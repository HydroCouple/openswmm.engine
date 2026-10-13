// SPDX-License-Identifier: Apache-2.0
#include "LidNodeExchange.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <type_traits>
#include "../data/SurfaceStateData.hpp"
#include "../solver/InertialEdges.hpp"

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

RichardsBedAcceptance acceptRichardsBedsAtFiring(SubsurfaceSolver& gw, SurfaceStateData& surface,
    double start, double end, const std::vector<RichardsBedOwner>& input) {
    auto fail=[](const std::string& error){RichardsBedAcceptance r;r.error=error;return r;};
    const double dt=end-start;
    if(!std::isfinite(start) || start<0 || !std::isfinite(end) || !std::isfinite(dt) || dt<=0 || input.empty())
        return fail("Invalid Richards joint owner firing interval.");
    if(!gw.richards_clock_valid_)
        return fail("Restored groundwater requires the R5 joint owner clock/restart extension.");
    if(gw.options_.per_subcatch || surface.transport.active() || gw.gather_fraction_!=1 ||
       surface.infil_rate.size()!=static_cast<std::size_t>(gw.state_.n_cells))
        return fail("Richards joint acceptance requires an idle water-only mesh owner boundary.");
    auto owners=input;const auto& beds=gw.node_beds_;
    std::set<int> selected,cells;
    for(const auto& owner:owners) {
        if(owner.bed<0 || owner.bed>=int(beds.size()) || !owner.column ||
           owner.max_water.size()!=owner.column->water.size() || owner.max_water.empty() ||
           !selected.insert(owner.bed).second)
            return fail("Invalid Richards hydraulic owner/bounds.");
        for(std::size_t k=0;k<owner.max_water.size();++k)
            if(!nonnegative(owner.max_water[k]) || !nonnegative(owner.column->water[k]) ||
               owner.column->water[k]>owner.max_water[k])
                return fail("Initial Richards water exceeds its hydraulic owner bounds.");
        const int cell=beds[owner.bed].cell;
        if(cell<0 || cell>=gw.state_.n_cells || gw.cell_completed_[cell]!=start ||
           (gw.source_owned_[cell] && gw.source_cell_end_[cell]!=start))
            return fail("Richards firing is stale or has discontinuous groundwater/source owner clocks.");
        if(gw.source_owned_[cell] && !cells.count(cell)) {
            double intake=0;
            for(const auto& receipt:gw.source_receipts_[cell]) {
                if(receipt.request.cell!=cell || !nonnegative(receipt.volume) || !receipt.mass.empty() ||
                   !std::isfinite(receipt.start) || receipt.start<start || !std::isfinite(receipt.end) ||
                   receipt.end<=receipt.start || receipt.end>end)
                    return fail("Invalid, future or unqualified paired source receipt in Richards firing.");
                intake+=receipt.volume;
            }
            if(!nonnegative(gw.state_.xacc_from_surface[cell]) || !std::isfinite(intake) ||
               std::abs(intake-gw.state_.xacc_from_surface[cell])>
                   1e-12*std::max({1.,intake,std::abs(gw.state_.xacc_from_surface[cell])}))
                return fail("Unpaired source water in Richards groundwater firing.");
            if(gw.source_completed_end_!=end)
                return fail("Richards firing requires the exact completed R4 source horizon.");
        }
        if(gw.state_.nacc[owner.bed]!=0)
            return fail("Richards bed has an unpaired pending generic-node receipt.");
        cells.insert(cell);
    }
    // A GW cell advances once, together with all of its already-bound porous
    // owners. Omitting one would silently advance its receiving boundary clock.
    for(std::size_t b=0;b<beds.size();++b)
        if(cells.count(beds[b].cell) && gw.richards_bed_owned_[b] && !selected.count(int(b)))
            return fail("Richards owner firing omitted another bound bed in the same cell.");
    std::sort(owners.begin(),owners.end(),[&](const auto& a,const auto& b){return beds[a.bed].node<beds[b.bed].node;});
    std::vector<RichardsBedBinding> bindings;
    for(const auto& o:owners)bindings.push_back({o.bed,o.column,o.invert,o.max_water.back(),o.clogging});
    RichardsBedAcceptance result;result.trial=trialRichardsBeds(gw,dt,bindings);
    if(!result.trial.error.empty())return fail(result.trial.error);
    for(std::size_t j=0;j<owners.size();++j)
        for(std::size_t k=0;k<owners[j].max_water.size();++k)
            if(!nonnegative(result.trial.columns[j].water[k]) ||
               result.trial.columns[j].water[k]>owners[j].max_water[k])
                return fail("Accepted Richards trial exceeds final hydraulic owner bounds; retry the interval.");

    // All work, including the normal closure's refund/return paths, stays on
    // private copies. Reviewed water-only source receipts share this firing;
    // species callbacks and live wells remain unqualified.
    auto candidate=gw;auto candidate_surface=surface;
    candidate.source_firing_end_=end;
    std::map<int,long double> added;
    for(const auto& receipt:result.trial.receipts) {
        const double net=receipt.accepted_up-receipt.accepted_down; // native GW sign
        candidate.state_.nacc[receipt.bed]=net;added[receipt.cell]+=net;
        candidate.richards_bed_owned_[receipt.bed]=true;
        candidate.richards_down_cum_[receipt.bed]+=receipt.accepted_down;
        candidate.richards_up_cum_[receipt.bed]+=receipt.accepted_up;
        candidate.bed_exchange_cum_[receipt.bed]+=net;
        // Do NOT also pay the generic mobile-node queue: water goes directly
        // into the accepted porous/surface column state below.
        if(!std::isfinite(candidate.richards_down_cum_[receipt.bed]) ||
           !std::isfinite(candidate.richards_up_cum_[receipt.bed]) ||
           !std::isfinite(candidate.bed_exchange_cum_[receipt.bed]))
            return fail("Richards cumulative receipt overflow.");
    }
    const auto storage=[](const SubsurfaceState& s,int i) {
        double unsat=s.hu[i];
        if(static_cast<GwClosure>(s.closure[i])==GwClosure::SIGMA)
            unsat=sigma::columnStorage(&s.theta_sigma[i],s.m_layers,s.n_cells,s.zs[i]-s.hg[i]);
        return (s.theta_s[i]*s.hg[i]+unsat)*s.area[i];
    };
    long double scale=1, balance=0;
    for(const int cell:cells) {
        double node=0,lateral=0;
        for(const auto b:gw.cell_beds_[cell])node+=gw.state_.nacc[b];
        for(int p=gw.edges_->cell_ptr[cell];p<gw.edges_->cell_ptr[cell+1];++p) {
            const auto e=gw.edges_->cell_edge[p];
            lateral+=gw.edges_->cell_sign[p]>0?gw.state_.eacc_L[e]:gw.state_.eacc_R[e];
        }
        const double initial=storage(gw.state_,cell);
        const double incoming=lateral+gw.state_.lacc[cell]+gw.state_.xacc_from_surface[cell];
        const double deep=candidate.state_.led_deep,et=candidate.state_.led_et;
        const long refunds=candidate.node_refunds_;
        const auto return_count=candidate.source_returns_.size();
        try { candidate.fireCell(cell,dt,candidate_surface,start); }
        catch(const std::exception& error) { return fail(error.what()); }
        candidate.cell_completed_[cell]=end; // the caller's exact accepted endpoint
        if(candidate.source_owned_[cell])candidate.source_cell_end_[cell]=end;
        const double expected=node+static_cast<double>(added[cell]);
        if(candidate.node_refunds_!=refunds ||
           std::abs(candidate.state_.qnode_last[cell]*dt-expected)>
               8*std::numeric_limits<double>::epsilon()*std::max(1.0,std::abs(expected)))
            return fail("Groundwater did not accept the matching Richards receipt; retry the interval.");
        double returned=candidate.state_.xacc_to_surface[cell]-gw.state_.xacc_to_surface[cell];
        for(std::size_t j=return_count;j<candidate.source_returns_.size();++j) {
            auto& r=candidate.source_returns_[j];
            if(r.intake.request.cell!=cell || !nonnegative(r.volume) || !r.mass.empty() ||
               !std::isfinite(r.available_at) || std::abs(r.available_at-end)>
                   8*std::numeric_limits<double>::epsilon()*std::max(1.,end))
                return fail("Invalid paired surface return in Richards groundwater firing.");
            r.available_at=end;
            if(r.kind!=SurfaceReturnKind::NUMERICAL_REFUND)r.intake.end=end;
            returned+=r.volume;
        }
        const double residual=initial+incoming-expected-(candidate.state_.led_deep-deep)-
            (candidate.state_.led_et-et)-returned-storage(candidate.state_,cell);
        const double cell_scale=std::max(1.0,initial+std::max(0.0,incoming)+std::max(0.0,-expected));
        if(!std::isfinite(residual) || std::abs(residual)>1.e-10*cell_scale)
            return fail("Groundwater Richards owner firing failed its water balance.");
        balance+=residual;scale+=cell_scale;
    }
    for(std::size_t j=0;j<owners.size();++j) {
        balance+=result.trial.reports[j].balance;
        for(double w:owners[j].column->water)scale+=w;
    }
    result.balance=static_cast<double>(balance);
    if(!std::isfinite(result.balance) || std::abs(balance)>1.e-10*scale)
        return fail("Joint Richards owner acceptance failed its water balance.");
    // Finish allocations BEFORE committing; all three owner updates are no-throw.
    std::vector<std::vector<double>> water;
    for(const auto& c:result.trial.columns)water.push_back(c.water);
    static_assert(std::is_nothrow_move_assignable_v<SubsurfaceSolver>);
    static_assert(std::is_nothrow_move_assignable_v<SurfaceStateData>);
    gw=std::move(candidate);surface=std::move(candidate_surface);
    for(std::size_t j=0;j<owners.size();++j)owners[j].column->water.swap(water[j]);
    return result;
}

RichardsBedAcceptance acceptRichardsBedsForTier(SubsurfaceSolver& gw,SurfaceStateData& surface,
    int tier,double start,double end,const std::vector<RichardsBedOwner>& owners) {
    const auto fail=[](const std::string& e){RichardsBedAcceptance r;r.error=e;return r;};
    if(!gw.active() || !gw.richards_clock_valid_ || !gw.tierHasCells(tier) ||
       !std::isfinite(start) || start<0 || !std::isfinite(end) || end<=start || !std::isfinite(end-start))
        return fail("Invalid Richards groundwater tier firing.");
    if(gw.options_.per_subcatch || surface.transport.active() || gw.gather_fraction_!=1 ||
       surface.infil_rate.size()!=static_cast<std::size_t>(gw.state_.n_cells))
        return fail("Richards tier acceptance requires an idle water-only mesh owner boundary.");
    const auto& due=gw.cells_by_tier_[tier];std::set<int> cells(due.begin(),due.end()),selected,beds;
    std::set<const richards::Column*> columns;
    for(int cell:due) {
        if(gw.cell_completed_[cell]!=start || (gw.source_owned_[cell] && gw.source_cell_end_[cell]!=start))
            return fail("Groundwater tier firing has stale or discontinuous owner clocks.");
        if(gw.source_owned_[cell] && gw.source_completed_end_!=end)
            return fail("Groundwater tier firing requires the exact completed R4 source horizon.");
        SubsurfaceSolver::NodeWaterBudget budget;
        if(auto e=gw.nodeWaterBudget(cell,end-start,budget);!e.empty())return fail(e);
    }
    for(const auto& owner:owners) {
        if(owner.bed<0 || owner.bed>=int(gw.node_beds_.size()) || !owner.column ||
           !beds.insert(owner.bed).second || !columns.insert(owner.column).second ||
           !cells.count(gw.node_beds_[owner.bed].cell))
            return fail("Richards owner is duplicate, invalid or outside the due groundwater tier.");
        selected.insert(gw.node_beds_[owner.bed].cell);
    }
    for(int cell:due)for(int bed:gw.cell_beds_[cell])
        if(gw.richards_bed_owned_[bed] && !beds.count(bed))
            return fail("Groundwater tier firing omitted a bound Richards bed.");
    auto candidate=gw;auto surf=surface;
    std::vector<richards::Column> trials;trials.reserve(owners.size());
    for(const auto& o:owners)trials.push_back(*o.column);
    auto private_owners=owners;
    for(std::size_t j=0;j<owners.size();++j)private_owners[j].column=&trials[j];
    RichardsBedAcceptance result;
    if(!owners.empty()) {
        result=acceptRichardsBedsAtFiring(candidate,surf,start,end,private_owners);
        if(!result.error.empty())return result;
    }
    std::vector<int> ordinary;double other_node=0;
    for(int cell:due) {
        if(!selected.count(cell))ordinary.push_back(cell);
        for(int bed:gw.cell_beds_[cell])other_node+=gw.state_.nacc[bed];
    }
    candidate.source_firing_end_=end;
    try {candidate.fireGwCellList(ordinary,end-start,surf,start,end);}
    catch(const std::exception& e){return fail(e.what());}
    if(candidate.node_refunds_!=gw.node_refunds_)
        return fail("Groundwater tier refunded an already accepted node receipt; retry the interval.");
    for(int cell:ordinary) {
        candidate.cell_completed_[cell]=end;
        if(candidate.source_owned_[cell])candidate.source_cell_end_[cell]=end;
    }
    for(std::size_t j=gw.source_returns_.size();j<candidate.source_returns_.size();++j) {
        auto& r=candidate.source_returns_[j];
        if(!cells.count(r.intake.request.cell) || !nonnegative(r.volume) || !r.mass.empty() ||
           !std::isfinite(r.available_at) || std::abs(r.available_at-end)>
               8*std::numeric_limits<double>::epsilon()*std::max(1.,end))
            return fail("Invalid paired surface return in groundwater tier firing.");
        r.available_at=end;
        if(r.kind!=SurfaceReturnKind::NUMERICAL_REFUND)r.intake.end=end;
    }
    const auto returns=[](const SubsurfaceSolver& g){long double v=0;for(const auto& r:g.source_returns_)v+=r.volume;return v;};
    long double initial=gw.state_.liveStorage()+returns(gw),final=candidate.state_.liveStorage()+returns(candidate);
    long double inflow=0,et=0;
    for(std::size_t j=0;j<owners.size();++j) {
        for(double w:owners[j].column->water)initial+=w;
        for(double w:trials[j].water)final+=w;
    }
    for(const auto& r:result.trial.reports){inflow+=r.inflow;et+=r.evaporation;}
    result.balance=static_cast<double>(initial+inflow-final-other_node-et-
        (candidate.state_.led_et-gw.state_.led_et)-(candidate.state_.led_deep-gw.state_.led_deep));
    if(!std::isfinite(result.balance) || std::abs(result.balance)>1e-10*std::max(1.L,std::abs(initial)+inflow))
        return fail("Richards groundwater tier firing failed its combined water balance.");
    ++candidate.tier_firings_[tier];
    gw=std::move(candidate);surface=std::move(surf);
    for(std::size_t j=0;j<owners.size();++j)owners[j].column->water.swap(trials[j].water);
    return result;
}
}
