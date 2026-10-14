// SPDX-License-Identifier: Apache-2.0
#include "SourceResultHistory.hpp"
#include "CommonSourceStage.hpp"
#include "SubsurfaceSolver.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../../core/SimulationContext.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace openswmm::twoD {
namespace {
constexpr double ft3=.3048*.3048*.3048;
constexpr std::size_t maxValues=8*1024*1024;
void row(SourceResultTable& t,std::initializer_list<double> values,std::initializer_list<double> extra={}) {
    if(values.size()+extra.size()!=t.columns.size())throw std::runtime_error("Source result row differs from schema.");
    std::size_t i=0;for(double v:values)t.columns[i++].push_back(v);
    for(double v:extra)t.columns[i++].push_back(v);
}
bool index(double v,std::size_t n) { return std::isfinite(v)&&v>=0&&v<double(n)&&std::floor(v)==v; }
}
std::map<std::string,SourceResultTable> SourceResultHistory::schema(bool outfall_returns) {
    std::map<std::string,SourceResultTable> tables;
    const auto add=[&](const char* name,std::initializer_list<std::pair<const char*,const char*>> fields){
        auto& t=tables[name];for(const auto& f:fields){t.fields.push_back(f.first);t.units.push_back(f.second);t.columns.emplace_back();}
    };
    add("boundaries",{{"frame","1"},{"interval","1"},{"start","s"},{"end","s"},
        {"mesh_rain","m3"},{"mesh_evaporation","m3"},{"mesh_return","m3"}});
    add("source_water",{{"frame","1"},{"source","1"},{"group","1"},{"inside_fraction","1"},
        {"initial_storage","m3"},{"rain","m3"},{"evaporation","m3"},{"infiltration","m3"},
        {"spatial_infiltration","m3"},{"outside_infiltration","m3"},{"aquifer_return","m3"},
        {"outlet","m3"},{"captured","m3"},{"runon","m3"},{"pervious_return","m3"},{"storage","m3"}});
    add("group_water",{{"frame","1"},{"group","1"},{"pending","m3"},{"residual","m3"}});
    add("group_mass",{{"frame","1"},{"group","1"},{"species","1"},
        {"pending","native_concentration*m3"},{"residual","native_concentration*m3"}});
    add("source_mass",{{"frame","1"},{"source","1"},{"type","1"},{"unit","1"},{"species","1"},
        {"surface","native_concentration*m3"},{"pavement_or_imperv1","native_concentration*m3"},
        {"soil_or_pervious","native_concentration*m3"},{"storage","native_concentration*m3"},
        {"rain","native_concentration*m3"},{"spatial_infiltration","native_concentration*m3"},
        {"outside_infiltration","native_concentration*m3"},{"aquifer_return","native_concentration*m3"},
        {"outlet","native_concentration*m3"},{"removed","native_concentration*m3"},{"pending_routing","native_concentration*m3"}});
    add("lid_usages",{{"usage","1"},{"type","1"},{"unit","1"},{"source","1"},{"area","m2"}});
    add("footprints",{{"owner","1"},{"source","1"},{"type","1"},{"unit","1"},{"usage","1"},{"cell","1"},{"area","m2"}});
    add("receiver",{{"frame","1"},{"cell","1"},{"completed_time","s"},{"intake_refresh","s"},
        {"et_refresh","s"},{"soil_et_pending","m3"},{"potential_et","m3"},{"surface_et","m3"},
        {"soil_et","m3"},{"unused_et","m3"},{"rejected","m3"},{"dunne","m3"}});
    for(const auto* name:{"intake","pending_intake"}) {
        add(name,{{"frame","1"},{"row","1"},{"donor_kind","1"},{"source","1"},{"usage","1"},{"cell","1"},
            {"start","s"},{"end","s"},{"area","m2"},{"pond","m"},{"water_ceiling","m3"},{"candidate","m3"},{"volume","m3"}});
        add((std::string(name)+"_mass").c_str(),{{"frame","1"},{"row","1"},{"species","1"},{"mass","native_concentration*m3"}});
    }
    for(const auto* name:{"return","pending_return"}) {
        add(name,{{"frame","1"},{"row","1"},{"kind","1"},{"cell","1"},{"available_at","s"},{"adopted_at","s"},
            {"donor_kind","1"},{"source","1"},{"usage","1"},{"intake_start","s"},{"intake_end","s"},
            {"intake_volume","m3"},{"volume","m3"}});
        add((std::string(name)+"_mass").c_str(),{{"frame","1"},{"row","1"},{"species","1"},
            {"intake_mass","native_concentration*m3"},{"mass","native_concentration*m3"}});
    }
    add("et",{{"frame","1"},{"owner","1"},{"source","1"},{"type","1"},{"unit","1"},{"cell","1"},
        {"start","s"},{"end","s"},{"area","m2"},{"potential","m3"},{"evaporation","m3"},{"soil_demand","m3"},{"unused","m3"}});
    if(outfall_returns) {
        auto& water=tables.at("source_water");
        water.fields.push_back("outfall_return");water.units.push_back("m3");water.columns.emplace_back();
        add("outfall_clock",{{"frame","1"},{"completed_time","s"}});
        add("pending_outfall_return",{{"frame","1"},{"row","1"},{"source","1"},{"available_at","s"},{"volume","m3"}});
    }
    return tables;
}
void SourceResultHistory::initializeIdentity(const CommonSourceStage& stage) {
    const auto& s=*stage.sources_;const auto& ctx=s.context();const auto& names=s.pollutantNames();const int np=int(names.size());
    sources_.clear();mass_units_.clear();
    outfall_returns_=s.acceptsOutfallReturns();
    tables_=schema(outfall_returns_);stage_=&stage;model_=stage.checkpoint_model_;
    for(int sc=0;sc<ctx.n_subcatches();++sc)sources_.push_back(ctx.subcatch_names.name_of(sc));
    pollutants_=names;
    for(int p=0;p<np;++p) {
        switch(ctx.pollutants.units.at(p)) {
        case MassUnits::MG_PER_L:mass_units_.push_back("mg/L*m3");break;
        case MassUnits::UG_PER_L:mass_units_.push_back("ug/L*m3");break;
        case MassUnits::COUNTS_PER_L:mass_units_.push_back("count/L*m3");break;
        default:throw std::runtime_error("Unknown pollutant units for source results.");
        }
    }
    for(std::size_t id=0;id<s.lids().usageOrder().size();++id) {
        const auto [t,u]=s.lids().usageOrder()[id];const auto& g=s.lids().group(t);
        row(tables_.at("lid_usages"),{double(id),double(t),double(u),double(g.subcatch_idx[u]),g.area[u]*.3048*.3048});
    }
    for(int cell:stage.cells())row(tables_.at("footprints"),{0,-1,-1,-1,-1,double(cell),stage.weather_[cell]});
    for(std::size_t i=0;i<s.atmosphere().size();++i) {
        const auto& a=s.atmosphere()[i];
        const double owner=a.kind==runoff::SourceEtKind::PERVIOUS?1:a.kind==runoff::SourceEtKind::IMPERVIOUS?2:3;
        double usage=-1,total=0;
        if(owner==3)for(std::size_t u=0;u<s.lids().usageOrder().size();++u)
            if(s.lids().usageOrder()[u]==std::pair{a.type,a.unit})usage=double(u);
        for(const auto& [cell,area]:stage.et_contacts_[i]) {
            row(tables_.at("footprints"),{owner,double(a.source),double(a.type),double(a.unit),usage,double(cell),area});total+=area;
        }
        const double outside=a.area*.3048*.3048-total;
        if(outside>1e-12)row(tables_.at("footprints"),{owner,double(a.source),double(a.type),double(a.unit),usage,-1,outside});
    }
}
void SourceResultHistory::appendRows(const CommonSourceStage& stage,std::size_t frame,double start) {
        const auto& s=*stage.sources_;const auto& ctx=s.context();const auto& names=s.pollutantNames();
        const int np=int(names.size());
        auto& ts=tables_;const double end=stage.completedEnd();
        row(ts.at("boundaries"),{double(frame),double(stage.intervals()),start,end,stage.meshRain(),stage.meshEvaporation(),stage.meshReturn()});
        if(outfall_returns_) {
            row(ts.at("outfall_clock"),{double(frame),s.outfallReturnEnd()});
            for(const auto& q:s.pendingOutfallReturns()) {
                auto& pending=ts.at("pending_outfall_return");
                row(pending,{double(frame),double(pending.rows()),double(q.source),q.available,q.volume*ft3});
            }
        }
        for(std::size_t group=0;group<s.clocks().groups().size();++group) {
            row(ts.at("group_water"),{double(frame),double(group),s.pendingVolume(int(group))*ft3,s.balanceResidual(int(group))*ft3});
            for(int p=0;p<np;++p)row(ts.at("group_mass"),{double(frame),double(group),double(p),s.pendingPollutantMass(int(group),p),s.pollutantBalanceResidual(int(group),p)});
            for(int sc:s.clocks().groups()[group].subcatches) {
                const auto& b=s.ledgers()[sc];
                // Keep the profile-off schema and old column positions intact.
                auto& water=ts.at("source_water");
                const std::initializer_list<double> values{double(frame),double(sc),double(group),s.spatialFractions()[sc],
                    b.initial_storage*ft3,b.rain*ft3,b.evaporation*ft3,b.infiltration*ft3,b.spatial_infiltration*ft3,
                    b.outside_infiltration*ft3,b.aquifer_return*ft3,b.outlet*ft3,b.captured*ft3,b.runon*ft3,b.pervious_return*ft3,s.storedVolume(sc)*ft3};
                if(outfall_returns_)row(water,values,{b.outfall_return*ft3});
                else row(water,values);
            }
        }
        for(const auto& b:s.pollutants())for(int p=0;p<np;++p) {
            const auto& m=b.layers[p];row(ts.at("source_mass"),{double(frame),double(b.source),double(b.type),double(b.unit),double(p),
                m[0],m[1],m[2],m[3],b.rain[p],b.spatial_infiltration[p],b.outside_infiltration[p],b.aquifer_return[p],b.outlet[p],b.removed[p],b.pending_routing[p]});
        }
        const auto sourceId=[&](const SurfaceIntakeReceipt& r) {
            if(r.request.kind==SurfaceDonorKind::MESH)return -1;
            for(std::size_t sc=0;sc<sources_.size();++sc)if(sources_[sc]==r.request.source)return int(sc);
            throw std::runtime_error("Unknown source result donor.");
        };
        const auto intake=[&](const char* table,const SurfaceIntakeReceipt& r) {
            auto& t=ts.at(table);const double id=double(t.rows());const auto& q=r.request;
            row(t,{double(frame),id,double(q.kind),double(sourceId(r)),double(q.unit),double(q.cell),r.start,r.end,q.area,q.pond,q.water_ceiling,q.candidate,r.volume});
            for(int p=0;p<np;++p)row(ts.at(std::string(table)+"_mass"),{double(frame),id,double(p),r.mass.at(p)});
        };
        const auto returned=[&](const char* table,const SurfaceReturnReceipt& r,double adopted) {
            auto& t=ts.at(table);const double id=double(t.rows());const bool refund=r.kind==SurfaceReturnKind::NUMERICAL_REFUND;
            row(t,{double(frame),id,double(r.kind),double(r.intake.request.cell),r.available_at,adopted,
                refund?double(r.intake.request.kind):-1,refund?double(sourceId(r.intake)):-1,
                refund?double(r.intake.request.unit):-1,refund?r.intake.start:0,refund?r.intake.end:0,
                refund?r.intake.volume:0,r.volume});
            for(int p=0;p<np;++p)row(ts.at(std::string(table)+"_mass"),{double(frame),id,double(p),refund?r.intake.mass.at(p):0,r.mass.at(p)});
        };
        for(const auto& r:stage.receipts())intake("intake",r);
        for(const auto& r:stage.returns())returned("return",r,start);
        for(const auto& r:stage.gw_->sourceReturns())returned("pending_return",r,-1);
        const auto& g=stage.gw_->state();
        for(int cell:stage.cells()) {
            for(const auto& r:stage.gw_->pendingSourceReceipts(cell))intake("pending_intake",r);
            row(ts.at("receiver"),{double(frame),double(cell),stage.gw_->sourceCompletedTime(cell),std::isnan(g.infil_refresh[cell])?-1:g.infil_refresh[cell],std::isnan(g.et_refresh[cell])?-1:g.et_refresh[cell],
                g.et_pending[cell],g.et_potential_cumulative[cell],g.et_surface_cumulative[cell],g.et_soil_cumulative[cell],
                g.et_unused_cumulative[cell],g.reject_cumulative[cell],g.dunne_cumulative[cell]});
        }
        for(const auto& r:stage.etReceipts())row(ts.at("et"),{double(frame),double(r.owner),double(r.source),double(r.type),double(r.unit),
            double(r.cell),r.start,r.end,r.area,r.potential,r.evaporation,r.soil_demand,r.unused});
}
SourceResultHistory SourceResultHistory::projection(const CommonSourceStage& stage,std::size_t frame,double start) {
    const auto error=stage.checkpointBoundary();
    if(!error.empty())throw std::runtime_error(error);
    if(stage.checkpoint_model_!=stage.checkpointModel())throw std::runtime_error("Source result model changed after attachment.");
    SourceResultHistory out;out.initializeIdentity(stage);out.appendRows(stage,frame,start);return out;
}
std::string SourceResultHistory::append(const CommonSourceStage& stage) {
    try {
        const auto error=stage.checkpointBoundary();if(!error.empty())return error;
        if(stage.checkpoint_model_!=stage.checkpointModel())return "Source result model changed after attachment.";
        if(!tables_.empty()&&(!stage_||stage_!=&stage||model_!=stage.checkpoint_model_))return "Source result recorder belongs to a different or saved run.";
        const std::size_t frame=tables_.empty()?0:tables_.at("boundaries").rows();
        const double start=frame?tables_.at("boundaries").columns[3].back():0;
        if(stage.intervals()!=static_cast<long>(frame)||(frame==0&&stage.completedEnd()!=0)||
           (frame>0&&stage.completedEnd()<=start))return "Capture initial state and every accepted source interval exactly once.";
        auto trial=*this; // Qualification recorder: strong failure guarantee, not the optimized production writer.
        if(frame==0)trial.initializeIdentity(stage);
        trial.appendRows(stage,frame,start);
        const auto anchor=stage.checkpointBytes(trial.boundary_);if(!anchor.empty())return anchor;
        const auto invalid=trial.validate();if(!invalid.empty())return invalid;
        *this=std::move(trial);return {};
    } catch(const std::exception& e){return e.what();}
}
std::string SourceResultHistory::refreshCheckpointMatch(const CommonSourceStage& stage) {
    if(stage_!=&stage||tables_.empty()||tables_.at("boundaries").rows()!=std::size_t(stage.intervals()+1)||
       tables_.at("boundaries").columns[3].back()!=stage.completedEnd())return "Source checkpoint match is not at the captured final boundary.";
    std::string bytes;const auto error=stage.checkpointBytes(bytes);if(!error.empty())return error;
    boundary_=std::move(bytes);return {};
}
std::string SourceResultHistory::extends(const SourceResultHistory& prefix) const {
    if(outfall_returns_!=prefix.outfall_returns_||sources_!=prefix.sources_||pollutants_!=prefix.pollutants_||mass_units_!=prefix.mass_units_||tables_.size()!=prefix.tables_.size())
        return "Source result prefix identities differ.";
    for(const auto& [name,t]:prefix.tables_) {
        const auto found=tables_.find(name);if(found==tables_.end())return "Source result prefix table is missing.";
        const auto& next=found->second;
        if(t.fields!=next.fields||t.units!=next.units||t.columns.size()!=next.columns.size())return "Source result prefix schema differs.";
        for(std::size_t c=0;c<t.columns.size();++c)if(next.columns[c].size()<t.columns[c].size()||
            !std::equal(t.columns[c].begin(),t.columns[c].end(),next.columns[c].begin()))return "Source result prefix changed or was truncated.";
        if((name=="footprints"||name=="lid_usages")&&t.columns!=next.columns)return "Source result ownership prefix changed.";
    }
    if(tables_.at("boundaries").rows()==prefix.tables_.at("boundaries").rows()&&boundary_!=prefix.boundary_)
        return "Source result boundary changed without a new interval.";
    return {};
}
std::string SourceResultHistory::resume(const CommonSourceStage& stage) {
    try {
        const auto invalid=validate();if(!invalid.empty())return invalid;
        if(boundary_.empty())return "Saved source history has no checkpoint match for continuation.";
        std::string bytes;const auto error=stage.checkpointBytes(bytes);if(!error.empty())return error;
        if(outfall_returns_!=stage.sources().acceptsOutfallReturns())
            return "Source history hydraulic-return capability differs from restored source profile.";
        if(bytes!=boundary_)return "Source history does not match the restored component checkpoint.";
        const auto n=tables_.at("boundaries").rows();
        if(stage.intervals()!=static_cast<long>(n-1)||stage.completedEnd()!=tables_.at("boundaries").columns[3].back())
            return "Source history final clock differs from restored state.";
        SourceResultHistory identity;identity.initializeIdentity(stage);
        if(sources_!=identity.sources_ || pollutants_!=identity.pollutants_ || mass_units_!=identity.mass_units_ ||
           tables_.at("footprints").columns!=identity.tables_.at("footprints").columns ||
           tables_.at("lid_usages").columns!=identity.tables_.at("lid_usages").columns)
            return "Source history identities or ownership differ from checkpoint.";
        auto trial=*this;
        if(n==1)trial=SourceResultHistory{};
        else {
            for(auto& [name,t]:trial.tables_)if(name!="footprints"&&name!="lid_usages") {
                auto at=std::find(t.columns[0].begin(),t.columns[0].end(),double(n-1));
                const auto keep=std::size_t(at-t.columns[0].begin());for(auto& col:t.columns)col.resize(keep);
            }
            trial.stage_=&stage;trial.model_=stage.checkpoint_model_;
        }
        const auto captured=trial.append(stage);if(!captured.empty())return captured;
        const auto differs=trial.extends(*this);if(!differs.empty())return differs;
        for(const auto& [name,t]:tables_)if(t.columns!=trial.tables_.at(name).columns)return "Source history final diagnostics differ from checkpoint.";
        *this=std::move(trial);return {};
    } catch(const std::exception& e){return e.what();}
}
std::string SourceResultHistory::resumeCapturedBoundary(const CommonSourceStage& stage) {
    try {
        auto trial=*this;
        const auto error=stage.inspectCheckpointBytes(boundary_,[&](const CommonSourceStage& captured){return trial.resume(captured);});
        if(!error.empty())return error;
        trial.stage_=&stage;trial.model_=stage.checkpoint_model_;
        *this=std::move(trial);return {};
    } catch(const std::exception& e){return e.what();}
}
std::string SourceResultHistory::validate() const {
    try {
        const auto expected=schema(outfall_returns_);if(tables_.size()!=expected.size())return "Missing source result tables.";
        std::size_t total=0;
        for(const auto& [name,t]:tables_) {
            const auto& s=expected.at(name);
            if(t.fields!=s.fields||t.units!=s.units||t.columns.size()!=s.columns.size())return "Source result schema or units differ.";
            for(std::size_t c=0;c<t.columns.size();++c) {
                if(t.columns[c].size()!=t.rows()||t.columns[c].size()>maxValues-total)return "Source result shapes or size differ.";
                total+=t.columns[c].size();
                for(double v:t.columns[c]) {
                    if(!std::isfinite(v))return "Nonfinite source result.";
                    const auto& f=t.fields[c];
                    const bool sentinel=f=="source"||f=="type"||f=="unit"||f=="usage"||f=="cell"||f=="donor_kind"||f=="adopted_at"||f=="intake_refresh"||f=="et_refresh";
                    if(f!="residual"&&(v<0&&(!sentinel||v!=-1)))return "Invalid signed source result.";
                    if(t.units[c]=="1"&&f!="inside_fraction"&&std::floor(v)!=v)return "Nonintegral source result identity.";
                }
            }
        }
        for(const auto* names:{&sources_,&pollutants_,&mass_units_}) {
            if(names->size()>65536)return "Too many source result names.";
            std::set<std::string> unique;
            for(const auto& name:*names) {
                if(name.empty()||name.size()>=512||name.find('\0')!=std::string::npos)return "Invalid source result name.";
                if(names!=&mass_units_&&!unique.insert(name).second)return "Duplicate source result name.";
            }
        }
        if(mass_units_.size()!=pollutants_.size())return "Missing source pollutant units.";
        for(const auto& u:mass_units_)if(u!="mg/L*m3"&&u!="ug/L*m3"&&u!="count/L*m3")return "Unknown source pollutant units.";
        const auto& b=tables_.at("boundaries").columns;const auto nf=b[0].size();
        if(nf==0)return "Source result history is empty.";
        for(std::size_t i=0;i<nf;++i)if(b[0][i]!=double(i)||b[1][i]!=double(i)||b[2][i]!=(i?b[3][i-1]:0)||
            (i?b[3][i]<=b[2][i]:b[3][i]!=0))return "Source result interval history is not contiguous.";
        for(const auto& [name,t]:tables_)if(name!="lid_usages"&&name!="footprints") {
            for(std::size_t r=0;r<t.rows();++r) {
                if(!index(t.columns[0][r],nf))return "Invalid source result frame.";
                const auto frame=std::size_t(t.columns[0][r]);
                for(std::size_t c=0;c<t.columns.size();++c) {
                    const auto& f=t.fields[c];const double v=t.columns[c][r];
                    if(f=="source"&&v!=-1&&!index(v,sources_.size()))return "Invalid source result source.";
                    if(f=="species"&&!index(v,pollutants_.size()))return "Invalid source result species.";
                    if(name!="outfall_clock"&&name!="pending_outfall_return"&&
                       (f=="end"||f=="available_at"||f=="completed_time"||f=="intake_refresh"||f=="et_refresh")&&v>b[3][frame])return "Source result looks beyond accepted time.";
                }
            }
        }
        // Endpoint tables must carry the same object/species identities at
        // every boundary. Missing rows cannot silently turn into plotted zeros.
        const auto complete=[&](const char* name,std::initializer_list<int> ids){
            const auto& t=tables_.at(name).columns;std::vector<std::set<std::vector<double>>> keys(nf);
            for(std::size_t r=0;r<t[0].size();++r) {
                std::vector<double> key;for(int id:ids)key.push_back(t[id][r]);
                if(!keys[std::size_t(t[0][r])].insert(key).second)return false;
            }
            for(const auto& key:keys)if(key!=keys[0])return false;
            return true;
        };
        const bool mesh_only=tables_.at("source_water").rows()==0 && tables_.at("group_water").rows()==0 &&
            std::all_of(tables_.at("footprints").columns[0].begin(),tables_.at("footprints").columns[0].end(),[](double owner){return owner==0;});
        if((!mesh_only && (tables_.at("source_water").rows()==0||tables_.at("group_water").rows()==0))||tables_.at("receiver").rows()==0||
           !complete("source_water",{1})||!complete("group_water",{1})||!complete("receiver",{1})||
           !complete("source_mass",{1,2,3,4})||!complete("group_mass",{1,2}))return "Incomplete source result endpoint history.";
        const auto& usages=tables_.at("lid_usages").columns;
        for(std::size_t r=0;r<usages[0].size();++r)if(usages[0][r]!=double(r)||usages[1][r]<0||usages[2][r]<0||
            !index(usages[3][r],sources_.size()))return "Invalid source result LID usage.";
        const auto lidIdentity=[&](double source,double type,double unit){
            for(std::size_t u=0;u<usages[0].size();++u)if(usages[3][u]==source&&usages[1][u]==type&&usages[2][u]==unit)return true;
            return false;
        };
        const auto& water=tables_.at("source_water").columns;
        const auto& gw=tables_.at("group_water").columns;
        for(std::size_t r=0;r<water[0].size();++r) {
            if(water[1][r]<0||water[2][r]<0||water[3][r]>1||
               std::abs(water[7][r]-water[8][r]-water[9][r])>1e-10*std::max(1.,water[7][r]))return "Invalid source water partition.";
            bool found=false;for(std::size_t k=0;k<gw[0].size();++k)if(gw[0][k]==water[0][r]&&gw[1][k]==water[2][r])found=true;
            if(!found)return "Source result clock group missing.";
        }
        for(std::size_t r=0;r<gw[0].size();++r) {
            double residual=-gw[2][r],scale=1;bool found=false;
            for(std::size_t k=0;k<water[0].size();++k)if(water[0][k]==gw[0][r]&&water[2][k]==gw[1][r]) {
                const double outfall=outfall_returns_?water[16][k]:0;
                found=true;residual+=water[4][k]+water[5][k]+water[10][k]+outfall-water[6][k]-water[7][k]-water[11][k]-water[15][k];
                scale+=water[4][k]+water[5][k]+water[10][k]+outfall;
            }
            if(!found||std::abs(residual-gw[3][r])>1e-10*scale)return "Source water residual differs from saved stores and transfers.";
        }
        if(outfall_returns_) {
            if(!pollutants_.empty())return "Outfall source results require water-only sources.";
            const auto& clock=tables_.at("outfall_clock").columns;
            const auto& pending=tables_.at("pending_outfall_return").columns;
            if(clock[0].size()!=nf)return "Missing outfall source result clocks.";
            for(std::size_t f=0;f<nf;++f)if(clock[0][f]!=double(f)||
                (f?clock[1][f]<clock[1][f-1]:clock[1][f]!=0))return "Invalid outfall source result clock.";
            std::vector<std::map<double,double>> amounts(nf);
            std::vector<std::set<std::pair<double,double>>> identities(nf);
            for(std::size_t r=0;r<pending[0].size();++r) {
                const auto f=std::size_t(pending[0][r]);
                if(pending[1][r]!=double(r)||pending[2][r]<0||pending[4][r]<=0||
                   pending[3][r]>clock[1][f]||b[3][f]-pending[3][r]>1e-12*std::max(1.,b[3][f])||
                   (r&&pending[0][r]==pending[0][r-1]&&pending[3][r]<pending[3][r-1])||
                   !identities[f].insert({pending[2][r],pending[3][r]}).second)
                    return "Invalid pending outfall source return.";
                amounts[f][pending[2][r]]+=pending[4][r];
            }
            std::map<double,double> previous;
            std::vector<std::map<double,double>> groups(nf);
            for(std::size_t r=0;r<water[0].size();++r) {
                const auto f=std::size_t(water[0][r]);const double source=water[1][r],credit=water[16][r];
                const auto found=amounts[f].find(source);const double queued=found==amounts[f].end()?0:found->second;
                if(credit<previous[source]||queued-credit>1e-10*std::max(1.,credit)||(f==0&&credit!=0))
                    return "Outfall source return exceeds cumulative credits.";
                previous[source]=credit;groups[f][water[2][r]]+=queued;
                if(found!=amounts[f].end())amounts[f].erase(found);
            }
            for(const auto& unknown:amounts)if(!unknown.empty())return "Pending outfall source return has no owned source.";
            for(std::size_t r=0;r<gw[0].size();++r)if(groups[std::size_t(gw[0][r])][gw[1][r]]-gw[2][r]>1e-10*std::max(1.,gw[2][r]))
                return "Pending outfall source return exceeds group storage.";
        }
        const auto& sm=tables_.at("source_mass").columns;const auto& gm=tables_.at("group_mass").columns;
        for(std::size_t r=0;r<sm[0].size();++r) {
            if(sm[1][r]<0||(sm[2][r]==-1?sm[3][r]!=-1||sm[8][r]!=0:!lidIdentity(sm[1][r],sm[2][r],sm[3][r])))return "Invalid source mass pool.";
        }
        if(gm[0].size()!=gw[0].size()*pollutants_.size())return "Missing source group mass.";
        for(std::size_t r=0;r<gm[0].size();++r) {
            double residual=-gm[3][r],scale=1;bool found=false;
            for(std::size_t k=0;k<water[0].size();++k)if(water[0][k]==gm[0][r]&&water[2][k]==gm[1][r]) {
                for(std::size_t z=0;z<sm[0].size();++z)if(sm[0][z]==gm[0][r]&&sm[1][z]==water[1][k]&&sm[4][z]==gm[2][r]) {
                    found=true;residual+=sm[9][z]+sm[12][z]-sm[10][z]-sm[11][z]-sm[13][z]-sm[14][z]-sm[5][z]-sm[6][z]-sm[7][z]-sm[8][z];
                    scale+=sm[9][z]+sm[12][z];
                }
            }
            if(!found||std::abs(residual-gm[4][r])>1e-10*scale)return "Source mass residual differs from saved pools and transfers.";
        }
        const auto& receiver=tables_.at("receiver").columns;
        const auto hasCell=[&](double cell){return std::find(receiver[1].begin(),receiver[1].end(),cell)!=receiver[1].end();};
        const auto& footprints=tables_.at("footprints").columns;
        for(std::size_t r=0;r<footprints[0].size();++r) {
            if(!index(footprints[0][r],4)||(footprints[5][r]!=-1&&!hasCell(footprints[5][r]))||
                (footprints[0][r]==0?footprints[1][r]!=-1||footprints[5][r]<0:!index(footprints[1][r],sources_.size()))||
                (footprints[0][r]==3?(!lidIdentity(footprints[1][r],footprints[2][r],footprints[3][r])||
                    !index(footprints[4][r],usages[0].size())||usages[1][std::size_t(footprints[4][r])]!=footprints[2][r]||
                    usages[2][std::size_t(footprints[4][r])]!=footprints[3][r]):
                    footprints[2][r]!=-1||footprints[3][r]!=-1||footprints[4][r]!=-1))return "Invalid reviewed source result footprint.";
        }
        for(std::size_t r=0;r<receiver[0].size();++r)if(receiver[1][r]<0||
            std::abs(receiver[6][r]-receiver[7][r]-receiver[8][r]-receiver[9][r]-receiver[5][r])>1e-10*std::max(1.,receiver[6][r]))return "Source receiver ET budget differs.";
        for(const auto* table:{"intake","pending_intake","return","pending_return"}) {
            const auto& t=tables_.at(table).columns;const bool isReturn=std::string(table).find("return")!=std::string::npos;
            const bool pending=std::string(table).find("pending")!=std::string::npos;
            const auto& m=tables_.at(std::string(table)+"_mass").columns;
            if(m[0].size()!=t[0].size()*pollutants_.size())return "Unpaired source result mass rows.";
            for(std::size_t r=0;r<t[0].size();++r) {
                if(t[1][r]!=double(r)||!index(t[2][r],3))return "Invalid source result receipt identity.";
                const auto frame=std::size_t(t[0][r]);const double v=t[12][r];
                if(isReturn) {
                    if(!hasCell(t[3][r])||t[4][r]>b[3][frame]||(pending?t[5][r]!=-1:(t[5][r]!=b[2][frame]||t[4][r]>t[5][r])))return "Invalid source return timing.";
                    if(t[2][r]==0&&(!index(t[6][r],3)||(t[6][r]==0)!=(t[7][r]==-1)||
                        (t[6][r]==2?(!index(t[8][r],usages[0].size())||usages[3][std::size_t(t[8][r])]!=t[7][r]):t[8][r]!=-1)||t[10][r]<=t[9][r]||t[10][r]>t[4][r]||v>t[11][r]*(1+1e-10)))return "Invalid numerical refund pairing.";
                } else {
                    if(!hasCell(t[5][r])||t[7][r]<=t[6][r]||(!pending&&(t[6][r]<b[2][frame]||t[7][r]>b[3][frame]))||v>std::min(t[10][r],t[11][r])+1e-10*std::max(1.,v))return "Invalid source intake timing or volume.";
                    if((t[2][r]==0)!=(t[3][r]==-1)||
                       (t[2][r]==2?(!index(t[4][r],usages[0].size())||usages[3][std::size_t(t[4][r])]!=t[3][r]):t[4][r]!=-1))return "Invalid source intake donor.";
                }
                for(std::size_t p=0;p<pollutants_.size();++p) {
                    const auto k=r*pollutants_.size()+p;
                    if(m[0][k]!=t[0][r]||m[1][k]!=double(r)||m[2][k]!=double(p)||(v==0&&m.back()[k]!=0))return "Invalid paired source mass identity.";
                    if(isReturn&&t[2][r]==0&&std::abs(m[4][k]-(t[11][r]>0?m[3][k]*v/t[11][r]:0))>1e-10*std::max(1.,m[4][k]))return "Source refund mass differs from original donor.";
                }
            }
        }
        const auto& et=tables_.at("et").columns;
        for(std::size_t r=0;r<et[0].size();++r) {
            const auto frame=std::size_t(et[0][r]);
            if(!index(et[1][r],4)||(et[5][r]!=-1&&!hasCell(et[5][r]))||(et[1][r]==0?et[2][r]!=-1||et[5][r]<0:et[2][r]<0)||
                (et[1][r]==3?!lidIdentity(et[2][r],et[3][r],et[4][r]):et[3][r]!=-1||et[4][r]!=-1)||et[6][r]<b[2][frame]||et[7][r]>b[3][frame]||et[7][r]<=et[6][r]||
                std::abs(et[9][r]-et[10][r]-et[11][r]-et[12][r])>1e-10*std::max(1.,et[9][r]))return "Source ET history does not close.";
        }
        return {};
    } catch(const std::exception& e){return e.what();}
}
}
