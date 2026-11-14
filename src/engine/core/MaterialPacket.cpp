// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "MaterialPacket.hpp"
#include "SplitFrame.hpp"
#include "openswmm_api_common.hpp"
#include "../quality/PeerPassiveStorageExchange.hpp"
#include <openswmm/engine/openswmm_split_material.h>
#include <bit>
#include <charconv>
#include <set>
#include <stdexcept>
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
#include <nlohmann/json.hpp>
namespace {
using J=nlohmann::json;
using namespace openswmm;
using Book=quality::PassiveStorageExchange;
constexpr auto schema="openswmm.node-surface-water/10";
constexpr std::size_t maxBytes=SWMM_MATERIAL_PACKET_MAX_BYTES;
void need(bool ok,const char* message){if(!ok)throw std::invalid_argument(message);}
void keys(const J& j,std::initializer_list<const char*> names){
    need(j.is_object() && j.size()==names.size(),"Material packet object fields differ");
    for(auto n:names)need(j.contains(n),"Material packet field missing");
}
template<class T> T integer(const J& j){
    need(j.is_number_integer(),"Material integer required");
    if(j.is_number_unsigned())need(j.get<std::uint64_t>()<=std::uint64_t(std::numeric_limits<T>::max()),"Material integer overflow");
    else need(j.get<std::int64_t>()>=std::numeric_limits<T>::min() &&
        j.get<std::int64_t>()<=std::numeric_limits<T>::max(),"Material integer overflow");
    return j.get<T>();
}
std::uint64_t id(const J& j){
    need(j.is_string(),"Material uint64 identity must be a decimal string");const auto& s=j.get_ref<const std::string&>();
    need(!s.empty() && s.size()<=20 && (s.size()==1 || s[0]!='0'),"Noncanonical material identity");
    for(char c:s)need(c>='0'&&c<='9',"Invalid material identity");
    std::uint64_t value=0;auto r=std::from_chars(s.data(),s.data()+s.size(),value);
    need(r.ec==std::errc()&&r.ptr==s.data()+s.size(),"Material identity overflow");return value;
}
std::string uid(std::uint64_t n){return std::to_string(n);}
double number(const J& j){need(j.is_number_float()&&std::isfinite(j.get<double>()),"Finite binary64 material value required");return j.get<double>();}
template<class T> T scalar(const J& j){
    if constexpr(std::is_same_v<T,bool>){need(j.is_boolean(),"Material Boolean required");return j.get<bool>();}
    else if constexpr(std::is_integral_v<T>)return integer<T>(j);
    else return number(j);
}
std::vector<double> column(const J& j,std::size_t n,bool empty=false){
    need(j.is_array()&&(j.size()==n||(empty&&j.empty())),"Material column extent differs");
    std::vector<double> out;out.reserve(j.size());for(const auto& v:j)out.push_back(number(v));return out;
}
void nonnegative(const std::vector<double>& v){for(double x:v)need(x>=0,"Negative material stock");}
std::string bits(double value){
    need(std::isfinite(value),"Nonfinite material value");std::string s(16,'0');auto x=std::bit_cast<std::uint64_t>(value);
    constexpr char digits[]="0123456789abcdef";for(int i=15;i>=0;--i){s[i]=digits[x&15];x>>=4;}return s;
}
J wire(J j,int depth=0){
    need(depth<=32,"Material encoding depth exceeded");
    if(j.is_number_float())return J{{"f64",bits(j.get<double>())}};
    if(j.is_array()){need(j.size()<=65536,"Material array limit exceeded");for(auto& v:j)v=wire(std::move(v),depth+1);}
    else if(j.is_object())for(auto& v:j.items())v.value()=wire(std::move(v.value()),depth+1);
    else if(j.is_string())need(j.get_ref<const std::string&>().size()<=8*1024*1024,"Material string limit exceeded");
    else need(j.is_boolean()||j.is_number_integer(),"Invalid material value type");
    return j;
}
quality::NativeAmountSum amount(const J& j){
    keys(j,{"parts"});const auto& parts=j.at("parts");need(parts.is_array()&&parts.size()<=64,"Material expansion limit exceeded");
    quality::NativeAmountSum sum;
    for(const auto& x:parts){const double v=number(x);need(v!=0,"Zero native amount component");sum.add(v);}
    need(parts.empty()||!sum.zero(),"Zero amount requires an empty expansion");return sum;
}
quality::NativeAmountSum rowAmount(const J& j){
    auto sum=amount(j);const auto& parts=j.at("parts");
    need(sum.parts().size()==parts.size(),"Noncanonical receipt row expansion");
    for(std::size_t k=0;k<parts.size();++k)need(sum.parts()[k]==number(parts[k]),"Noncanonical receipt row expansion");
    return sum;
}
J unwire(J j){
    if(j.is_object()&&j.contains("f64")){
        keys(j,{"f64"});need(j.at("f64").is_string(),"Invalid binary64 encoding");auto s=j.at("f64").get<std::string>();
        need(s.size()==16,"Invalid binary64 width");std::uint64_t x=0;
        for(char c:s){need((c>='0'&&c<='9')||(c>='a'&&c<='f'),"Noncanonical binary64 encoding");x=(x<<4)|std::uint64_t(c<='9'?c-'0':c-'a'+10);}
        const double v=std::bit_cast<double>(x);need(std::isfinite(v),"Nonfinite material value");return v;
    }
    need(!j.is_number_float()&&!j.is_null(),"Material doubles require binary64 bit strings");
    if(j.is_array()){need(j.size()<=65536,"Material array limit exceeded");for(auto& v:j)v=unwire(std::move(v));}
    else if(j.is_object()){
        for(auto& v:j.items())v.value()=unwire(std::move(v.value()));
        if(j.contains("parts"))amount(j);
    }else if(j.is_string())need(j.get_ref<const std::string&>().size()<=8*1024*1024,"Material string limit exceeded");
    return j;
}
J parse(const char* bytes,std::size_t size){
    need(bytes&&size&&size<=maxBytes,"Invalid material packet size");need(!std::memchr(bytes,0,size),"Embedded material packet NUL");
    std::vector<std::set<std::string>> objects;std::size_t values=0;
    J::parser_callback_t check=[&](int depth,J::parse_event_t event,J& value){
        need(depth<=32&&++values<=3000000,"Material parser depth/work limit exceeded");
        if(event==J::parse_event_t::object_start)objects.emplace_back();
        if(event==J::parse_event_t::key)need(!objects.empty()&&objects.back().insert(value.get<std::string>()).second,"Duplicate material packet key");
        if(event==J::parse_event_t::object_end)objects.pop_back();return true;
    };
    return unwire(J::parse(bytes,bytes+size,check));
}
J exact(const quality::NativeAmountSum& x){J p=J::array();for(double d:x.parts())p.push_back(d);return J{{"parts",std::move(p)}};}
J sample(const Book::Sample& s){return J{{"volume_ft3",s.volume},{"depth_ft",s.depth},{"concentration",s.concentration}};}
J point(const Book::HydraulicPoint& p){return J{{"point",uid(p.point)},{"cell",p.cell},{"node",p.node},{"exchange_m3",p.exchange_si}};}
J points(const std::vector<Book::HydraulicPoint>& p){need(p.size()<=4096,"Material point limit exceeded");J j=J::array();for(const auto& x:p)j.push_back(point(x));return j;}
J leg(const Book::AppliedLeg& x){return J{{"point",uid(x.point)},{"cell",x.cell},{"node",x.node},{"drain",x.drain},{"applied",x.applied},
    {"water_m3",x.water_si},{"source_amount",x.mass_si},{"water_ft3",x.water_native},{"native_amount",x.mass_native},
    {"before",x.before},{"after",x.after},{"sample",x.sample},{"start_ms",x.batch_start_ms},{"end_ms",x.batch_end_ms},
    {"phase",int(x.phase)},{"volume_before_m3",x.volume_before},{"volume_after_m3",x.volume_after},
    {"offset_s",x.offset_s},{"duration_s",x.duration_s},{"withdrawal_m3",x.withdrawal_si}};}
J legs(const std::vector<Book::AppliedLeg>& l){need(l.size()<=65536,"Material leg limit exceeded");J j=J::array();for(const auto& x:l)j.push_back(leg(x));return j;}
J view(const quality::PeerFundingView& v){
    const auto& a=v.allowance;need(v.nodes.size()<=4096&&a.pools.size()==v.nodes.size(),"Material pool limit exceeded");
    std::size_t budget=0;for(const auto& name:v.nodes){need(name.size()<=maxBytes/8,"Material name limit exceeded");budget+=name.size();}
    for(const auto* c:{&v.head,&v.volume,&v.depth,&v.full_volume,&v.rpt_full_volume,&v.ponded_area})need(c->size()==v.nodes.size(),"Material view column extent differs");
    for(const auto& p:a.pools)budget+=1024+40*(p.roundoff.parts().size()+p.sealed.parts().size()+p.granted.parts().size()+p.spent.parts().size());
    need(budget<=maxBytes/2&&a.pollutant.size()<=1024*1024,"Material view work limit exceeded");
    J pools=J::array();for(const auto& p:a.pools)pools.push_back(J{{"node",p.node},{"settled",sample(p.settled)},
        {"published_concentration",p.published_concentration},{"roundoff",exact(p.roundoff)},{"sealed",exact(p.sealed)},
        {"granted",exact(p.granted)},{"spent",exact(p.spent)}});
    J allowance={{"ordinal",uid(a.ordinal)},{"receipt",uid(a.receipt)},{"generation",uid(a.generation)},
        {"phase",int(a.phase)},{"capture_ms",a.capture_ms},{"start_ms",a.start_ms},{"end_ms",a.end_ms},{"remaining_s",a.remaining},
        {"pollutant",a.pollutant},{"units",int(a.units)},{"leg_limit",uid(a.leg_limit)},{"pools",std::move(pools)},
        {"spills",legs(a.spills)},{"reconciled",a.reconciled},{"peer_reported",a.peer_reported},
        {"peer",{{"issuer",uid(a.peer.issuer)},{"recipient",uid(a.peer.recipient)},{"session",uid(a.peer.session)}}}};
    return J{{"allowance",std::move(allowance)},{"points",points(v.points)},{"si_to_native",v.si_to_native},{"nodes",v.nodes},
        {"head_ft",v.head},{"volume_ft3",v.volume},{"depth_ft",v.depth},{"full_volume_ft3",v.full_volume},
        {"rim_volume_ft3",v.rpt_full_volume},{"ponded_area_ft2",v.ponded_area}};
}
J basis(const std::string& species,MassUnits units,double factor){
    need(int(units)>=0&&int(units)<=2,"Unsupported material basis");const char* names[]={"MG/L","UG/L","#/L"};std::string u=names[int(units)];
    return J{{"pollutant",species},{"class","BULK"},{"units",int(units)},{"concentration_basis",u},
        {"native_amount_basis",u+"*ft3"},{"surface_amount_basis",u+"*m3"},{"si_to_native",factor},
        {"law","changing-volume"},{"material_arithmetic","exact-slices"},{"water_arithmetic","retain-aggregates"}};
}
J envelope(const quality::PeerFundingView& v,std::uint64_t seq,int phase,int stage,J body){
    const auto& a=v.allowance;return J{{"schema",schema},{"stage",stage},{"node_count",v.nodes.size()},
        {"basis",basis(a.pollutant,a.units,v.si_to_native)},{"outer_phase",phase},{"outer_sequence",uid(seq)},
        {"issuer",uid(a.peer.issuer)},{"recipient",uid(a.peer.recipient)},{"session",uid(a.peer.session)},{"body",std::move(body)}};
}
std::string text(J j){auto out=wire(std::move(j)).dump();need(out.size()<=maxBytes,"Encoded material packet exceeds byte limit");return out;}
void validatePoints(const J& j,std::size_t nodes,bool zero){
    need(j.is_array()&&!j.empty()&&j.size()<=4096,"Invalid material point map");
    for(std::size_t k=0;k<j.size();++k){const auto& p=j[k];keys(p,{"point","cell","node","exchange_m3"});
        const int n=integer<int>(p.at("node"));need(id(p.at("point"))==k&&integer<int>(p.at("cell"))>=0&&n>=0&&std::size_t(n)<nodes,"Invalid material point order/topology");
        const double x=number(p.at("exchange_m3"));need(!zero||x==0,"Unexpected material geometry booking");}
}
void validateView(const J& v,const SimulationContext& ctx){
    keys(v,{"allowance","points","si_to_native","nodes","head_ft","volume_ft3","depth_ft","full_volume_ft3","rim_volume_ft3","ponded_area_ft2"});
    const auto n=std::size_t(ctx.n_nodes());need(v.at("nodes")==ctx.node_names.names()&&number(v.at("si_to_native"))>0,"Material node identity/conversion differs");
    for(auto k:{"head_ft","volume_ft3","depth_ft","full_volume_ft3","rim_volume_ft3","ponded_area_ft2"}){
        auto c=column(v.at(k),n);if(std::string_view(k)!="head_ft")nonnegative(c);}
    validatePoints(v.at("points"),n,true);const auto& a=v.at("allowance");
    keys(a,{"ordinal","receipt","generation","phase","capture_ms","start_ms","end_ms","remaining_s","pollutant","units","leg_limit","pools","spills","reconciled","peer_reported","peer"});
    need(id(a.at("ordinal"))&&id(a.at("receipt"))&&id(a.at("leg_limit"))<=65536,"Invalid material window identity/capacity");id(a.at("generation"));
    const int phase=integer<int>(a.at("phase"));need(phase>=1&&phase<=3&&number(a.at("capture_ms"))>=0&&number(a.at("start_ms"))>=0&&
        number(a.at("end_ms"))>number(a.at("start_ms"))&&number(a.at("remaining_s"))>=0,"Invalid material window interval");
    need(a.at("pollutant")==ctx.pollutant_names.names()[0]&&integer<int>(a.at("units"))==int(ctx.pollutants.units[0]),"Material species/basis differs");
    need(a.at("reconciled").is_boolean()&&!a.at("reconciled").get<bool>()&&a.at("peer_reported").is_boolean()&&!a.at("peer_reported").get<bool>()&&
        a.at("spills").is_array()&&a.at("spills").empty(),"Unexpected material allowance state");
    const auto& peer=a.at("peer");keys(peer,{"issuer","recipient","session"});need(id(peer.at("issuer"))&&id(peer.at("recipient"))&&id(peer.at("session"))&&peer.at("issuer")!=peer.at("recipient"),"Invalid material peer identity");
    const auto& pools=a.at("pools");need(pools.is_array()&&pools.size()==n,"Invalid material pool extent");
    for(std::size_t k=0;k<n;++k){const auto& p=pools[k];keys(p,{"node","settled","published_concentration","roundoff","sealed","granted","spent"});
        need(integer<int>(p.at("node"))==int(k)&&number(p.at("published_concentration"))>=0,"Invalid material pool order");
        const auto& s=p.at("settled");keys(s,{"volume_ft3","depth_ft","concentration"});need(number(s.at("volume_ft3"))>0&&number(s.at("depth_ft"))>0&&number(s.at("concentration"))>=0,"Invalid settled material pool");
        amount(p.at("roundoff"));for(auto field:{"sealed","granted","spent"})need(!amount(p.at(field)).negative(),"Negative gross material amount");
        need(amount(p.at("granted")).atLeast(amount(p.at("spent"))),"Material spend exceeds described grant");}
}
void balance(J& j,SimulationContext::MassBalance2D& b,bool read){
#define FIELD(name) if(read)b.name=scalar<decltype(b.name)>(j.at(#name));else j[#name]=b.name;
    FIELD(init_storage) FIELD(final_storage) FIELD(external_in) FIELD(external_out) FIELD(rainfall_in)
    FIELD(coupling_1d_to_2d_in) FIELD(coupling_2d_to_1d_out) FIELD(outfall_in) FIELD(outfall_out)
    FIELD(boundary_in) FIELD(boundary_out) FIELD(evap_out) FIELD(infil_out) FIELD(infil_to_aquifer)
    FIELD(infil_aquifer_pending) FIELD(aquifer_in) FIELD(active) FIELD(solver_nsteps) FIELD(solver_nrhs)
    FIELD(solver_avg_h) FIELD(solver_last_h) FIELD(solver_active_min) FIELD(solver_active_mean) FIELD(solver_active_max) FIELD(solver_n_tiers)
#undef FIELD
    if(read){need(j.size()==26,"Material balance fields differ");const auto& a=j.at("tier_cells");need(a.is_array()&&a.size()==8,"Invalid material tier extent");for(int k=0;k<8;++k)b.solver_tier_cells[k]=integer<long>(a[k]);}
    else j["tier_cells"]=b.solver_tier_cells;
}
J boundary(const SplitFrame& f,const SimulationContext& ctx){
    need(f.material&&f.settled_reports&&!f.source_count&&!f.aquifer&&!f.spatial_sources&&!f.source_returns&&f.lid_nodes.empty()&&!f.extended_water&&!f.reviewed_exchange,"Not a closed material boundary frame");
    need(f.node_count>0&&f.node_count<=4096&&f.model.size()<=8*1024*1024,"Material boundary limit exceeded");
    for(const auto* c:{&f.material_concentration,&f.material_full_volume,&f.material_ponded_area,&f.material_rate,&f.pond_area,&f.pond_band})need(c->size()<=std::size_t(f.node_count),"Material column limit exceeded");
    need(f.coupled.size()<=std::size_t(f.node_count)&&f.conductance.size()<=4096,"Material boundary work limit exceeded");
    J c;const auto& n=f.nodes;
#define COL(name) need(n.name.size()<=std::size_t(f.node_count),"Material column limit exceeded");c[#name]=n.name;
    COL(head) COL(depth) COL(volume) COL(rpt_full_volume) COL(coupling_queue) COL(coupling_volume) COL(ponded_area) COL(inflow) COL(outflow) COL(lat_flow)
#undef COL
    J b;auto copy=f.balance;balance(b,copy,false);
    J body={{"reply",f.reply},{"phase",f.phase},{"sequence",uid(f.sequence)},{"sender",uid(f.sender)},{"target",uid(f.target)},
        {"report_flush",f.report_flush},{"report_time_ms",f.report_time_ms},{"start_s",f.start},{"duration_s",f.dt},{"remaining_s",f.remaining},
        {"configuration",f.model},{"node_ids",ctx.node_names.names()},{"point_map",points(f.material_points)},
        {"concentrations",f.material_concentration},{"full_volume_ft3",f.material_full_volume},{"ponded_area_ft2",f.material_ponded_area},
        {"coupling_rate_cfs",f.material_rate},{"columns",std::move(c)},{"coupled",f.coupled},{"pond_area_ft2",f.pond_area},{"pond_band_ft",f.pond_band},
        {"balance",std::move(b)},{"conductance",f.conductance}};
    return J{{"schema",schema},{"stage",0},{"node_count",f.node_count},{"basis",basis(f.material_species,f.material_units,f.material_factor)},{"body",std::move(body)}};
}
std::unique_ptr<SplitFrame> readBoundary(const J& j,const SimulationContext& ctx){
    keys(j,{"schema","stage","node_count","basis","body"});const auto& b=j.at("body");
    keys(b,{"reply","phase","sequence","sender","target","report_flush","report_time_ms","start_s","duration_s","remaining_s","configuration","node_ids","point_map","concentrations","full_volume_ft3","ponded_area_ft2","coupling_rate_cfs","columns","coupled","pond_area_ft2","pond_band_ft","balance","conductance"});
    auto f=std::make_unique<SplitFrame>();f->material=f->settled_reports=true;f->node_count=ctx.n_nodes();const auto n=std::size_t(f->node_count);
    f->phase=integer<int>(b.at("phase"));need(f->phase==0||f->phase==1||f->phase==2||f->phase==3||f->phase==8,"Invalid material outer phase");
    need(b.at("reply").is_boolean()&&b.at("report_flush").is_boolean(),"Invalid material boundary flags");f->reply=b.at("reply").get<bool>();f->report_flush=b.at("report_flush").get<bool>();
    f->sender=id(b.at("sender"));f->target=id(b.at("target"));f->sequence=id(b.at("sequence"));
    need(f->sender&&(f->phase?f->target&&f->sender!=f->target&&f->sequence:!f->target&&!f->sequence&&f->reply),"Invalid material boundary identity");
    f->start=number(b.at("start_s"));f->dt=number(b.at("duration_s"));f->remaining=number(b.at("remaining_s"));f->report_time_ms=number(b.at("report_time_ms"));
    need(f->start>=0&&f->remaining>=0&&f->report_time_ms>=0&&((f->phase==1||f->phase==2)?f->dt>0:f->dt==0),"Invalid material boundary interval");
    need(b.at("configuration").is_string()&&b.at("node_ids")==ctx.node_names.names(),"Material model/node ordering differs");f->model=b.at("configuration").get<std::string>();
    const auto& m=j.at("basis");f->material_species=m.at("pollutant").get<std::string>();f->material_units=ctx.pollutants.units[0];f->material_factor=number(m.at("si_to_native"));
    if(!f->phase){validatePoints(b.at("point_map"),n,true);for(const auto& p:b.at("point_map"))f->material_points.push_back({id(p.at("point")),integer<int>(p.at("cell")),integer<int>(p.at("node")),number(p.at("exchange_m3"))});}
    else need(b.at("point_map").is_array()&&b.at("point_map").empty(),"Unexpected material phase point geometry");
    f->material_concentration=column(b.at("concentrations"),n);nonnegative(f->material_concentration);
    f->material_full_volume=column(b.at("full_volume_ft3"),n);nonnegative(f->material_full_volume);
    f->material_ponded_area=column(b.at("ponded_area_ft2"),n);nonnegative(f->material_ponded_area);
    f->material_rate=column(b.at("coupling_rate_cfs"),n);
    const auto& c=b.at("columns");keys(c,{"head","depth","volume","rpt_full_volume","coupling_queue","coupling_volume","ponded_area","inflow","outflow","lat_flow"});
#define COL(name) f->nodes.name=column(c.at(#name),n,true);
    COL(head) COL(depth) COL(volume) COL(rpt_full_volume) COL(coupling_queue) COL(coupling_volume) COL(ponded_area) COL(inflow) COL(outflow) COL(lat_flow)
#undef COL
    for(const auto* c:{&f->nodes.head,&f->nodes.depth,&f->nodes.volume,&f->nodes.rpt_full_volume,&f->nodes.coupling_queue,&f->nodes.coupling_volume,&f->nodes.inflow,&f->nodes.outflow})need(c->size()==n,"Incomplete material node columns");
    nonnegative(f->nodes.depth);nonnegative(f->nodes.volume);nonnegative(f->nodes.rpt_full_volume);
    const auto& coupled=b.at("coupled");need(coupled.is_array()&&(coupled.empty()||coupled.size()==n),"Invalid material coupled extent");for(const auto& v:coupled){auto x=integer<int>(v);need(x==0||x==1,"Invalid material coupled flag");f->coupled.push_back(x);}
    f->pond_area=column(b.at("pond_area_ft2"),n,true);f->pond_band=column(b.at("pond_band_ft"),n,true);
    if(!f->phase){
        need(f->coupled.size()==n&&f->nodes.ponded_area.size()==n&&f->pond_area.size()==f->pond_band.size()&&
            (f->pond_area.empty()||f->pond_area.size()==n),"Incomplete material geometry columns");
        nonnegative(f->nodes.ponded_area);nonnegative(f->pond_area);nonnegative(f->pond_band);
        std::vector<bool> mapped(n,false);for(const auto& p:f->material_points)mapped[p.node]=true;
        for(std::size_t k=0;k<n;++k)need(f->coupled[k]==1&&mapped[k]&&f->nodes.ponded_area[k]>0,"Unmapped material geometry node");
    }
    auto mb=b.at("balance");balance(mb,f->balance,true);const auto& g=b.at("conductance");need(g.is_array()&&g.size()<=4096,"Invalid material conductance extent");
    for(const auto& p:g){need(p.is_array()&&p.size()==2,"Invalid material conductance row");int node=integer<int>(p[0]);double value=number(p[1]);need(node>=0&&std::size_t(node)<n&&value>=0,"Invalid material conductance");f->conductance.emplace_back(node,value);}
    return f;
}
void validate(const J& j,const SimulationContext& ctx){
    need(j.is_object()&&j.at("schema")==schema&&ctx.n_nodes()>0&&ctx.n_nodes()<=4096&&ctx.n_pollutants()==1,"Material schema/model mismatch");
    need(integer<int>(j.at("node_count"))==ctx.n_nodes(),"Material node extent mismatch");
    const auto& b=j.at("basis");need(b==basis(ctx.pollutant_names.names()[0],ctx.pollutants.units[0],number(b.at("si_to_native")))&&number(b.at("si_to_native"))>0,"Material basis/policy mismatch");
    int stage=integer<int>(j.at("stage"));need(stage>=0&&stage<=3,"Unknown material packet stage");if(!stage){readBoundary(j,ctx);return;}
    keys(j,{"schema","stage","node_count","basis","outer_phase","outer_sequence","issuer","recipient","session","body"});
    need(id(j.at("issuer"))&&id(j.at("recipient"))&&id(j.at("session"))&&id(j.at("outer_sequence"))&&j.at("issuer")!=j.at("recipient"),"Invalid material transport identity");
    int phase=integer<int>(j.at("outer_phase"));need(phase==2||phase==3||phase==8,"Invalid material window phase");
    const auto& body=j.at("body");if(stage==1)keys(body,{"view"});else if(stage==2)keys(body,{"view","proof","rows","spend","queue_ft3","direct_ft3","remaining_s"});else keys(body,{"view","queue_ft3","remaining_s","committed"});
    const auto& v=body.at("view");validateView(v,ctx);const auto& a=v.at("allowance");
    need(a.at("peer").at("issuer")==j.at("issuer")&&a.at("peer").at("recipient")==j.at("recipient")&&a.at("peer").at("session")==j.at("session")&&
        integer<int>(a.at("phase"))==(phase==2?1:phase==8?2:3)&&v.at("si_to_native")==b.at("si_to_native"),"Material session/window binding differs");
    if(stage==1)return;
    column(body.at("queue_ft3"),ctx.n_nodes());need(number(body.at("remaining_s"))>=0,"Invalid material water span");
    if(stage==3){need(body.at("committed").is_boolean()&&body.at("committed").get<bool>(),"Material ACK must follow commitment");return;}
    for(double d:column(body.at("direct_ft3"),ctx.n_nodes()))need(d==0,"Unexpected direct material water booking");
    const auto& p=body.at("proof");keys(p,{"si_to_native","legs","points"});need(number(p.at("si_to_native"))==number(v.at("si_to_native")),"Material proof conversion differs");validatePoints(p.at("points"),ctx.n_nodes(),false);
    need(p.at("points").size()==v.at("points").size(),"Material proof topology extent differs");
    for(std::size_t k=0;k<p.at("points").size();++k)need(p.at("points")[k].at("cell")==v.at("points")[k].at("cell")&&p.at("points")[k].at("node")==v.at("points")[k].at("node"),"Material proof topology differs");
    const auto& l=p.at("legs");need(l.is_array()&&l.size()<=id(a.at("leg_limit")),"Material proof leg limit exceeded");
    for(const auto& x:l){keys(x,{"point","cell","node","drain","applied","water_m3","source_amount","water_ft3","native_amount","before","after","sample","start_ms","end_ms","phase","volume_before_m3","volume_after_m3","offset_s","duration_s","withdrawal_m3"});
        auto point=id(x.at("point"));need(point<v.at("points").size(),"Material leg point outside map");const auto& mapped=v.at("points")[point];
        need(integer<int>(x.at("cell"))==integer<int>(mapped.at("cell"))&&integer<int>(x.at("node"))==integer<int>(mapped.at("node"))&&
            x.at("drain").is_boolean()&&x.at("applied").is_boolean()&&x.at("applied").get<bool>()&&x.at("phase")==a.at("phase")&&x.at("start_ms")==a.at("start_ms")&&x.at("end_ms")==a.at("end_ms"),"Material applied leg binding differs");
        for(auto k:{"water_m3","source_amount","water_ft3","native_amount","before","after","sample","volume_before_m3","volume_after_m3","offset_s","duration_s","withdrawal_m3"})need(number(x.at(k))>=0,"Invalid material applied operand");}
    const auto& rows=body.at("rows");need(rows.is_array()&&rows.size()<=v.at("points").size(),"Material row limit exceeded");std::uint64_t previous=0;bool first=true;
    for(const auto& r:rows){keys(r,{"point","node","pollutant","units","drain_water","spill_water","drain_amount","spill_amount"});auto pt=id(r.at("point"));need(pt<v.at("points").size()&&(first||pt>previous)&&
        r.at("node")==v.at("points")[pt].at("node")&&r.at("pollutant")==a.at("pollutant")&&r.at("units")==a.at("units"),"Material row ordering/basis differs");first=false;previous=pt;
        for(auto k:{"drain_water","spill_water","drain_amount","spill_amount"})need(!rowAmount(r.at(k)).negative(),"Negative material row amount");}
    const auto& spent=body.at("spend");need(spent.is_array()&&spent.size()==std::size_t(ctx.n_nodes()),"Material spend extent differs");for(std::size_t k=0;k<spent.size();++k)need(!amount(spent[k]).negative()&&amount(a.at("pools")[k].at("granted")).atLeast(amount(spent[k])),"Material reported spend exceeds grant");
}
J checked(const char* p,std::size_t n,const SimulationContext& ctx){auto j=parse(p,n);validate(j,ctx);return j;}
}
namespace openswmm {
std::string materialBoundaryPacket(const SplitFrame& f,const SimulationContext& ctx){return text(boundary(f,ctx));}
std::unique_ptr<SplitFrame> materialBoundaryFromPacket(const char* p,std::size_t n,const SimulationContext& ctx){auto j=checked(p,n,ctx);need(integer<int>(j.at("stage"))==0,"Material boundary packet required");return readBoundary(j,ctx);}
std::string materialOfferPacket(const quality::PeerFundingView& v,std::uint64_t s,int p){return text(envelope(v,s,p,1,J{{"view",view(v)}}));}
std::string materialProofPacket(const quality::PeerFundingReply& r,std::uint64_t s,int p,const std::vector<double>& q,const std::vector<double>& d,double remaining){
    need(r.proof().legs.size()<=maxBytes/2048&&q.size()<=4096&&d.size()<=4096,"Material proof encoding work limit exceeded");
    J rows=J::array();need(r.rows().size()<=4096&&r.spend().size()<=4096,"Material proof row limit exceeded");
    for(const auto& x:r.rows())rows.push_back(J{{"point",uid(x.point)},{"node",x.node},{"pollutant",x.pollutant},{"units",int(x.units)},
        {"drain_water",exact(x.waterAmount(true))},{"spill_water",exact(x.waterAmount(false))},{"drain_amount",exact(x.materialAmount(true))},{"spill_amount",exact(x.materialAmount(false))}});
    J spend=J::array();for(const auto& a:r.spend())spend.push_back(exact(a));const auto& proof=r.proof();
    return text(envelope(r.view(),s,p,2,J{{"view",view(r.view())},{"proof",{{"si_to_native",proof.si_to_native},{"legs",legs(proof.legs)},{"points",points(proof.points)}}},
        {"rows",std::move(rows)},{"spend",std::move(spend)},{"queue_ft3",q},{"direct_ft3",d},{"remaining_s",remaining}}));
}
std::string materialAcknowledgementPacket(const quality::PeerFundingView& v,std::uint64_t s,int p,const std::vector<double>& q,double remaining){return text(envelope(v,s,p,3,J{{"view",view(v)},{"queue_ft3",q},{"remaining_s",remaining},{"committed",true}}));}
void matchMaterialPacket(const std::string& issued,const char* returned,std::size_t size,const SimulationContext& ctx){
    const auto expected=checked(issued.data(),issued.size(),ctx);const auto actual=checked(returned,size,ctx);
    need(wire(expected)==wire(actual),"Material packet differs from retained native issuance; session stopped");
}
}
#else
namespace openswmm {
std::string materialBoundaryPacket(const SplitFrame&,const SimulationContext&){throw std::invalid_argument("Material codec unavailable");}
std::unique_ptr<SplitFrame> materialBoundaryFromPacket(const char*,std::size_t,const SimulationContext&){throw std::invalid_argument("Material codec unavailable");}
std::string materialOfferPacket(const quality::PeerFundingView&,std::uint64_t,int){throw std::invalid_argument("Material codec unavailable");}
std::string materialProofPacket(const quality::PeerFundingReply&,std::uint64_t,int,const std::vector<double>&,const std::vector<double>&,double){throw std::invalid_argument("Material codec unavailable");}
std::string materialAcknowledgementPacket(const quality::PeerFundingView&,std::uint64_t,int,const std::vector<double>&,double){throw std::invalid_argument("Material codec unavailable");}
void matchMaterialPacket(const std::string&,const char*,std::size_t,const SimulationContext&){throw std::invalid_argument("Material codec unavailable");}
}
#endif
namespace {thread_local char materialError[2048]{};
int failure(int code,std::string_view message) noexcept {
    auto n=std::min(message.size(),sizeof materialError-1);
    if(n<message.size())while(n&&(static_cast<unsigned char>(message[n])&0xc0)==0x80)--n;
    std::memcpy(materialError,message.data(),n);materialError[n]=0;return code;
}
template<class Work>int api(SWMM_Engine e,Work work) noexcept {
    materialError[0]=0;if(!e)return failure(SWMM_ERR_BADHANDLE,"Null material engine");
    try{const int rc=work();return rc==SWMM_OK?rc:failure(rc,"Material interface operation unavailable or refused");}catch(const std::bad_alloc&){return failure(SWMM_ERR_NOMEM,"Material packet allocation failed");}
    catch(const std::exception& ex){return failure(SWMM_ERR_BADPARAM,ex.what());}
    catch(...){return failure(SWMM_ERR_INTERNAL,"Non-standard material codec exception");}
}
}
extern "C" {
const char* swmm_split_material_last_error(void){return materialError;}
int swmm_split_material_validate_input(SWMM_Engine e){
    return api(e,[&]() -> int {
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
        const auto& ctx=to_engine(e)->context();
        if(ctx.state!=openswmm::EngineState::OPENED)return failure(SWMM_ERR_LIFECYCLE,"Material eligibility requires an OPENED handle");
        openswmm::validateMaterialSplitInput(ctx);return SWMM_OK;
#else
        return SWMM_ERR_PLUGIN;
#endif
    });
}
int swmm_split_material_configure(SWMM_Engine e,int role){
    if(!e)return failure(SWMM_ERR_BADHANDLE,"Null material engine");
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
    return to_engine(e)->configureMaterialSplit(role);
#else
    return SWMM_ERR_PLUGIN;
#endif
}
int swmm_split_material_set_transport(SWMM_Engine e,SWMM_MaterialTransport cb,void* user){return e?to_engine(e)->setMaterialTransport(cb,user):SWMM_ERR_BADHANDLE;}
int swmm_split_material_bind(SWMM_Engine e,SWMM_Engine surface,std::uint64_t capacity){
    if(!e||!surface)return failure(SWMM_ERR_BADHANDLE,"Null material peer");if(!capacity||capacity>65536)return failure(SWMM_ERR_BADPARAM,"Material capacity must be 1..65536");
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
    return to_engine(e)->bindMaterialPeer(*to_engine(surface),std::size_t(capacity),true);
#else
    return SWMM_ERR_PLUGIN;
#endif
}
int swmm_split_material_packet_encode(SWMM_Engine e,SWMM_SplitFrame f,char** out){
    if(!e)return failure(SWMM_ERR_BADHANDLE,"Null material engine");if(!f||!out)return failure(SWMM_ERR_BADPARAM,"Missing material frame/output");
    return api(e,[&]{
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
        auto s=openswmm::materialBoundaryPacket(*static_cast<openswmm::SplitFrame*>(f),to_engine(e)->context());
        checked(s.data(),s.size(),to_engine(e)->context());auto p=std::make_unique<char[]>(s.size()+1);std::memcpy(p.get(),s.c_str(),s.size()+1);*out=p.release();return SWMM_OK;
#else
        return SWMM_ERR_PLUGIN;
#endif
    });
}
int swmm_split_material_packet_decode(SWMM_Engine e,const char* p,std::size_t n,SWMM_SplitFrame* out){
    if(!e)return failure(SWMM_ERR_BADHANDLE,"Null material engine");if(!p||!n||n>SWMM_MATERIAL_PACKET_MAX_BYTES||!out)return failure(SWMM_ERR_BADPARAM,"Invalid material packet/output size");
    return api(e,[&]{
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
        auto f=openswmm::materialBoundaryFromPacket(p,n,to_engine(e)->context());*out=f.release();return SWMM_OK;
#else
        return SWMM_ERR_PLUGIN;
#endif
    });
}
int swmm_split_material_packet_copy(SWMM_Engine e,const char* p,std::size_t n,char** out){
    if(!e)return failure(SWMM_ERR_BADHANDLE,"Null material engine");if(!p||!n||n>SWMM_MATERIAL_PACKET_MAX_BYTES||!out)return failure(SWMM_ERR_BADPARAM,"Invalid material packet/output size");
    return api(e,[&]{
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
        auto s=text(checked(p,n,to_engine(e)->context()));auto copy=std::make_unique<char[]>(s.size()+1);std::memcpy(copy.get(),s.c_str(),s.size()+1);*out=copy.release();return SWMM_OK;
#else
        return SWMM_ERR_PLUGIN;
#endif
    });
}
}
