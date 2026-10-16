// SPDX-License-Identifier: Apache-2.0
#include "RuntimeCouplingCheckpoint.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include "../data/MeshData.hpp"
#include "../../data/TableData.hpp"
#include <map>
#include <stdexcept>
#include <type_traits>
#include <tuple>

namespace openswmm::twoD {
namespace {
using Archive = detail::CompletedSourceArchive;
constexpr uint64_t maxBytes = 64 * 1024 * 1024;
static_assert(std::is_nothrow_move_assignable_v<RuntimeSources>);
static_assert(std::is_nothrow_move_assignable_v<RuntimeForcings>);
static_assert(std::is_nothrow_move_assignable_v<BoundaryData>);
struct State { RuntimeSources surface, groundwater; RuntimeForcings forcing; BoundaryData boundary; };
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
bool close(double a, double b) {
    return std::isfinite(a) && std::isfinite(b) &&
        std::abs(a-b) <= 1e-10 * std::max({1.0, std::abs(a), std::abs(b)});
}
uint32_t checksum(const std::string& bytes) {
    uint32_t crc = ~uint32_t(0);
    for (unsigned char c : bytes) {
        crc ^= c;
        for (int k=0; k<8; ++k) crc=(crc>>1)^((crc&1)?0xedb88320u:0u);
    }
    return ~crc;
}
bool ownerOk(const std::string& owner) {
    return owner.find('/') == std::string::npos && owner.find('\0') == std::string::npos;
}
bool localIdOk(const std::string& id) {
    return !id.empty() && id.find('\0') == std::string::npos &&
        id.rfind("provider/",0) != 0 && id.rfind("boundary/",0) != 0;
}
bool sourceIdOk(const RuntimeSource& row) {
    if (!ownerOk(row.owner)) return false;
    if (row.owner.empty()) return localIdOk(row.id);
    const auto prefix = "provider/" + row.owner + "/";
    return row.id.rfind(prefix,0) == 0 && localIdOk(row.id.substr(prefix.size()));
}
void bindingOk(const RuntimeWaterCheckpointBinding& b) {
    require(!b.identity.empty() && b.identity.size() < maxBytes &&
        !b.groundwater_cells.empty() && b.groundwater_cells.size() <= 1000000 &&
        std::isfinite(b.completed_time) && b.completed_time >= 0,
        "Invalid runtime water checkpoint binding.");
}
void receiptOk(const RuntimeReceipt& r, double time, bool outward_positive = false) {
    require(r.heat_requested == 0 && r.heat_applied == 0 &&
        r.requested_mass.empty() && r.applied_mass.empty(),
        "Runtime water checkpoint heat and species receipts are not qualified.");
    for (double v : {r.requested, r.applied, r.water_in, r.water_out, r.last_flow, r.last_dt})
        require(std::isfinite(v), "Nonfinite runtime water checkpoint receipt.");
    require(r.water_in >= 0 && r.water_out >= 0 && r.last_dt >= 0 && r.last_dt <= time &&
        close(r.applied, outward_positive ? r.water_out-r.water_in : r.water_in-r.water_out),
        "Invalid runtime water checkpoint receipt budget.");
    const double last = std::abs(r.last_flow) * r.last_dt;
    require(std::isfinite(last) && std::isfinite(r.water_in+r.water_out) &&
        last <= r.water_in+r.water_out+1e-10*std::max(1.0,r.water_in+r.water_out) &&
        (r.last_dt != 0 || r.last_flow == 0), "Invalid runtime water checkpoint last receipt.");
    if (time == 0) require(r.requested == 0 && r.applied == 0 && r.water_in == 0 && r.water_out == 0,
        "Initial runtime water checkpoint has accumulated receipts.");
}
void original(Archive& a, BoundaryData::RuntimeOriginal& v) {
    a(v.type,v.head,v.flow,v.slope,v.stage_ts,v.flow_ts,v.rating,v.stage_name,v.flow_name,v.rating_name);
}
BoundaryData::RuntimeOriginal descriptor(const BoundaryData& b,int k) {
    return {b.edge_bc_type.at(k),b.edge_bc_head.at(k),b.edge_bc_flow.at(k),b.edge_bed_slope.at(k),
        b.edge_bc_tseries.at(k),b.edge_bc_flow_tseries.at(k),b.edge_bc_rating_curve.at(k),
        b.edge_bc_tseries_name.at(k),b.edge_bc_flow_tseries_name.at(k),b.edge_bc_rating_curve_name.at(k)};
}
bool same(const BoundaryData::RuntimeOriginal& a,const BoundaryData::RuntimeOriginal& b) {
    return std::tie(a.type,a.head,a.flow,a.slope,a.stage_ts,a.flow_ts,a.rating,a.stage_name,a.flow_name,a.rating_name)==
        std::tie(b.type,b.head,b.flow,b.slope,b.stage_ts,b.flow_ts,b.rating,b.stage_name,b.flow_name,b.rating_name);
}
// Resolved values are work state; the authored signal and its table remain
// the fallback model. Clearing re-evaluates it before the next native flux.
int reference(int index,const std::string& name,const TableData* tables,bool timeseries) {
    if(index==-1) {require(name.empty(),"Unexpected authored boundary signal name.");return -1;}
    require(tables && index>=-2,"Boundary signal model is missing.");
    const int resolved=index==-2 ? tables->find_by_kind(name,timeseries) : index;
    require(resolved>=0 && resolved<int(tables->tables.size()) &&
        (name.empty() ? index>=0 : tables->find_by_kind(name,timeseries)==resolved),
        "Boundary signal name or table index differs.");
    const auto& t=tables->tables[resolved];
    require((t.type==TableType::TIMESERIES)==timeseries && !t.is_file_based && !t.file_handle &&
        t.num_cols==1 && !t.x.empty() && t.y.size()==t.x.size(),"Boundary signal requires a single-column in-memory table of its authored kind.");
    for(std::size_t i=0;i<t.x.size();++i)require(std::isfinite(t.x[i]) && std::isfinite(t.y[i]) &&
        (i==0 || t.x[i]>t.x[i-1]),"Invalid authored boundary signal records.");
    return resolved;
}
bool fallback(const BoundaryData::RuntimeOriginal& value,const BoundaryData::RuntimeOriginal& base,const TableData* tables) {
    auto expected=base,current=value;
    const auto signal=[&](int& a,int& b,const std::string& name,bool ts) {
        const int resolved=reference(b,name,tables,ts);
        if(a!=b && a!=resolved)return false;
        a=b;return true;
    };
    if(!signal(current.stage_ts,expected.stage_ts,base.stage_name,true) ||
       !signal(current.flow_ts,expected.flow_ts,base.flow_name,true) ||
       !signal(current.rating,expected.rating,base.rating_name,false))return false;
    if(base.stage_ts!=-1)current.head=base.head;
    if(base.flow_ts!=-1 || base.rating!=-1)current.flow=base.flow;
    return std::isfinite(value.head) && std::isfinite(value.flow) && same(current,expected);
}
void boundaryFields(Archive& a,BoundaryData& b) {
    a.fixed(b.edge_bc_type,b.edge_bed_slope,b.edge_bc_head,b.edge_bc_tseries,b.edge_bc_tseries_name,
        b.edge_bc_flow,b.edge_bc_flow_tseries,b.edge_bc_flow_tseries_name,b.edge_bc_rating_curve,b.edge_bc_rating_curve_name);
}
void boundaryOk(const BoundaryData& v,const RuntimeWaterCheckpointBinding& b,bool completed) {
    if(b.boundary_slots.empty()) return;
    require(b.authored_boundary && b.boundary_slots.size()==b.groundwater_cells.size()*kMaxCellVerts,
        "Invalid boundary checkpoint model binding.");
    const auto& authored=*b.authored_boundary;
    const auto shape=[&](const BoundaryData& data) {
        const auto n=b.boundary_slots.size();
        return data.edge_bc_type.size()==n && data.edge_bed_slope.size()==n && data.edge_bc_head.size()==n &&
            data.edge_bc_tseries.size()==n && data.edge_bc_tseries_name.size()==n && data.edge_bc_flow.size()==n &&
            data.edge_bc_flow_tseries.size()==n && data.edge_bc_flow_tseries_name.size()==n &&
            data.edge_bc_rating_curve.size()==n && data.edge_bc_rating_curve_name.size()==n;
    };
    require(shape(authored) && shape(v),"Boundary checkpoint descriptor shape differs.");
    require(v.edge_bc_cum_flux.size()==b.boundary_slots.size() && v.runtime_original.size()==v.runtime_owner.size(),
        "Boundary checkpoint flux shape or ownership differs.");
    for(std::size_t k=0;k<b.boundary_slots.size();++k) {
        const auto base=descriptor(authored,int(k)),current=descriptor(v,int(k));
        require(base.type>=int8_t(BoundaryType::WALL) && base.type<=int8_t(BoundaryType::RATING_CURVE) &&
            std::isfinite(base.head) && std::isfinite(base.flow) && std::isfinite(base.slope) && base.slope>=0 &&
            (b.boundary_slots[k] || base.type==int8_t(BoundaryType::WALL)) &&
            (base.type==int8_t(BoundaryType::SPECIFIED_STAGE) || (base.stage_ts==-1 && base.stage_name.empty())) &&
            (base.type==int8_t(BoundaryType::SPECIFIED_FLOW) || (base.flow_ts==-1 && base.flow_name.empty())) &&
            (base.type==int8_t(BoundaryType::RATING_CURVE) ? base.rating!=-1 : (base.rating==-1 && base.rating_name.empty())),
            "Invalid authored boundary fallback descriptor.");
        reference(base.stage_ts,base.stage_name,b.boundary_tables,true);
        reference(base.flow_ts,base.flow_name,b.boundary_tables,true);
        reference(base.rating,base.rating_name,b.boundary_tables,false);
        const auto old=v.runtime_original.find(int(k));
        if(old==v.runtime_original.end()) require(fallback(current,base,b.boundary_tables) && !v.runtime_owner.count(int(k)),
            "Boundary checkpoint changed its authored fallback without an override.");
        else require(b.boundary_slots[k] && fallback(old->second,base,b.boundary_tables) &&
            v.runtime_owner.count(int(k)) && ownerOk(v.runtime_owner.at(int(k))) && v.receipts.count(int(k)) &&
            (current.type==int8_t(BoundaryType::SPECIFIED_STAGE) || current.type==int8_t(BoundaryType::SPECIFIED_FLOW)) &&
            std::isfinite(current.head) && std::isfinite(current.flow) && current.slope==base.slope &&
            current.stage_ts==-1 && current.flow_ts==-1 && current.rating==-1 && current.stage_name.empty() &&
            current.flow_name.empty() && current.rating_name.empty(), "Invalid boundary checkpoint override or saved fallback.");
        require(std::isfinite(v.edge_bc_cum_flux[k]),"Nonfinite boundary checkpoint cumulative flux.");
        const auto receipt=v.receipts.find(int(k));
        require(!completed || (receipt!=v.receipts.end() ? close(v.edge_bc_cum_flux[k],-receipt->second.applied) : v.edge_bc_cum_flux[k]==0),
            "Boundary checkpoint edge flux and water receipt differ.");
    }
    for(const auto& [slot,old]:v.runtime_original) require(slot>=0 && slot<int(b.boundary_slots.size()) && b.boundary_slots[slot],
        "Boundary checkpoint override is not an exterior edge.");
    for(const auto& [slot,r]:v.receipts) {
        require(slot>=0 && slot<int(b.boundary_slots.size()) && b.boundary_slots[slot],
            "Boundary checkpoint receipt is not an exterior edge.");
        if(completed)receiptOk(r,b.completed_time);
    }
}
void expiryOk(double until, bool enabled, double time) {
    require((std::isfinite(until) && until > 0) || until == std::numeric_limits<double>::infinity(),
        "Invalid runtime water checkpoint expiry.");
    require(!enabled || until > time, "Runtime water checkpoint has an active record at or past its expiry.");
}
void sourceStore(RuntimeSources& store, const RuntimeWaterCheckpointBinding& b, bool check, bool groundwater) {
    RuntimeSources next; next.rows=store.rows;
    const int nc=static_cast<int>(b.groundwater_cells.size());
    for (std::size_t k=0; k<next.rows.size(); ++k) {
        const auto& r=next.rows[k];
        const bool boundary = groundwater && !b.boundary_slots.empty() && r.boundary_edge >= 0;
        require((boundary ? (ownerOk(r.owner) && r.id == "boundary/"+std::to_string(r.cell)+"/"+std::to_string(r.boundary_edge)) : sourceIdOk(r)) && r.cell >= 0 && r.cell < nc && std::isfinite(r.flow),
            "Invalid runtime water checkpoint source identity, cell or flow.");
        require(r.heat == 0 && r.concentrations.empty() && r.rates.empty(),
            "Runtime water checkpoint heat and species sources are not qualified.");
        if(boundary) require(r.boundary_edge < kMaxCellVerts &&
            b.boundary_slots.at(MeshData::slot(r.cell,r.boundary_edge)) &&
            (std::isnan(r.boundary_head) || std::isfinite(r.boundary_head)) &&
            r.until == std::numeric_limits<double>::infinity(),
            "Invalid groundwater boundary checkpoint edge, stage or validity.");
        else require(r.boundary_edge == -1 && std::isnan(r.boundary_head),
            "Runtime water checkpoint boundary profile is not qualified.");
        expiryOk(r.until,r.enabled,b.completed_time); receiptOk(r.receipt,b.completed_time);
        require(next.index[r.id].emplace(r.cell,k).second,
            "Duplicate runtime water checkpoint source identity.");
    }
    next.rebuild(nc);
    if (check) require(store.index == next.index &&
        (store.cells == next.cells || (store.rows.empty() && store.cells.empty())),
        "Runtime water checkpoint source lookup differs from its records.");
    store=std::move(next);
}
void validateState(State& s, const RuntimeWaterCheckpointBinding& b, bool check, bool completed = true) {
    bindingOk(b); sourceStore(s.surface,b,check,false); sourceStore(s.groundwater,b,check,true);
    boundaryOk(s.boundary,b,completed);
    if(!b.boundary_slots.empty()) {
        double incoming=0,outgoing=0;
        for(const auto& r:s.groundwater.rows) {incoming+=r.receipt.water_in;outgoing+=r.receipt.water_out;}
        require(std::isfinite(incoming) && std::isfinite(outgoing),"Nonfinite aggregate groundwater provider receipts.");
    }
    for (const auto& r:s.groundwater.rows) require(b.groundwater_cells[r.cell],
        "Runtime water checkpoint source has no groundwater receiver.");
    RuntimeForcings next; next.rows=s.forcing.rows;
    const int nc=static_cast<int>(b.groundwater_cells.size());
    std::map<int,std::string> uniform;
    for (std::size_t k=0; k<next.rows.size(); ++k) {
        const auto& r=next.rows[k];
        require(r.channel >= 1 && r.channel <= 3 && r.cell >= -1 && r.cell < nc &&
            r.mode >= 0 && r.mode <= 2 && (r.mode != 0 || !r.enabled) &&
            std::isfinite(r.rate) && r.rate >= 0 && ownerOk(r.owner),
            "Invalid runtime water checkpoint forcing.");
        require(r.concentrations.empty(), "Runtime water checkpoint rain quality is not qualified.");
        if (r.channel == 3) require(r.mode != 2 && r.cell >= 0 && b.groundwater_cells[r.cell],
            "Runtime water checkpoint infiltration needs a local groundwater receiver.");
        expiryOk(r.until,r.enabled,b.completed_time); receiptOk(r.receipt,b.completed_time,r.channel == 3);
        if (r.channel == 3) require(r.receipt.requested >= 0 && r.receipt.applied >= 0 &&
            r.receipt.applied <= r.receipt.requested+1e-10*std::max(1.0,r.receipt.requested) &&
            r.receipt.water_in == 0 && r.receipt.last_flow >= 0,
            "Invalid runtime water checkpoint paired infiltration receipt.");
        else require(r.receipt.requested == 0 && r.receipt.applied == 0 && r.receipt.water_in == 0 &&
            r.receipt.water_out == 0 && r.receipt.last_flow == 0 && r.receipt.last_dt == 0,
            "Runtime water checkpoint rain and ET do not have applied-volume receipts.");
        require(next.index[r.channel].emplace(r.cell,k).second,
            "Duplicate runtime water checkpoint forcing identity.");
        if (r.enabled && r.cell == -1) uniform.emplace(r.channel,r.owner);
    }
    for (const auto& r:next.rows) if (r.enabled && r.cell >= 0) {
        const auto u=uniform.find(r.channel);
        require(u == uniform.end() || u->second == r.owner,
            "Runtime water checkpoint forcing ownership overlaps.");
    }
    next.rebuild(nc);
    if (check) {
        const auto& old=s.forcing;
        require(old.index == next.index &&
            (old.cells.size() == std::size_t(nc) || (old.rows.empty() && old.cells.empty())),
            "Runtime water checkpoint forcing lookup differs from its records.");
        for (int k:old.uniform) require(k >= -1 && k < int(old.rows.size()),
            "Invalid runtime water checkpoint uniform lookup.");
        for (const auto& cell:old.cells) for (int k:cell) require(k >= -1 && k < int(old.rows.size()),
            "Invalid runtime water checkpoint cell lookup.");
        // Expiry leaves disabled forcing indices in place; compare the effective
        // lookups instead of requiring identical inactive cache entries.
        for (int c=0; c<nc; ++c) for (int ch=1; ch<=4; ++ch) {
            const auto* a=old.get(ch,c); const auto* n=next.get(ch,c);
            require((!a && !n) || (a && n && a-old.rows.data() == n-next.rows.data()),
                "Runtime water checkpoint effective forcing lookup differs.");
        }
    }
    s.forcing=std::move(next);
}
void receipt(Archive& a, RuntimeReceipt& r) {
    a(r.requested,r.applied,r.water_in,r.water_out,r.last_flow,r.last_dt);
}
void state(Archive& a, State& s, bool boundaries) {
    const auto source=[&](auto& r) { a(r.id,r.owner,r.cell,r.flow,r.until,r.enabled);
        if(boundaries)a(r.boundary_edge,r.boundary_head);receipt(a,r.receipt); };
    a.sequence(s.surface.rows,source); a.sequence(s.groundwater.rows,source);
    a.sequence(s.forcing.rows,[&](auto& r) {
        a(r.channel,r.cell,r.mode,r.rate,r.until,r.enabled,r.owner); receipt(a,r.receipt);
    });
    if(boundaries) {
        boundaryFields(a,s.boundary);a.fixed(s.boundary.edge_bc_cum_flux);
        struct Row {int slot=0;BoundaryData::RuntimeOriginal old{};std::string owner;};
        std::vector<Row> rows;
        for(const auto& [slot,old]:s.boundary.runtime_original)rows.push_back({slot,old,s.boundary.runtime_owner.at(slot)});
        a.sequence(rows,[&](auto& r){a(r.slot);original(a,r.old);a(r.owner);});
        std::vector<std::pair<int,RuntimeReceipt>> receipts(s.boundary.receipts.begin(),s.boundary.receipts.end());
        a.sequence(receipts,[&](auto& r){a(r.first);receipt(a,r.second);});
        if(a.reading()) {
            s.boundary.runtime_original.clear();s.boundary.runtime_owner.clear();s.boundary.receipts.clear();
            for(auto& r:rows) {require(s.boundary.runtime_original.emplace(r.slot,std::move(r.old)).second,"Duplicate boundary override.");
                s.boundary.runtime_owner.emplace(r.slot,std::move(r.owner));}
            for(auto& r:receipts)require(s.boundary.receipts.emplace(r.first,std::move(r.second)).second,"Duplicate boundary receipt.");
        }
    }
}
State privateState(const RuntimeWaterCheckpointBinding& b,const RuntimeSources& s,const RuntimeSources& g,
    const RuntimeForcings& f,const BoundaryData* boundary) {
    require(b.boundary_slots.empty() ? !boundary : boundary && b.authored_boundary,"Boundary checkpoint profile or target differs.");
    return {s,g,f,boundary?*boundary:BoundaryData{}};
}
}
std::string RuntimeWaterCheckpoint::boundaryModel(const BoundaryData& b,const TableData* tables) {
    auto copy=b;Archive a;boundaryFields(a,copy);std::vector<int> used;
    for(int k=0;k<b.size();++k)for(int i:{reference(b.edge_bc_tseries.at(k),b.edge_bc_tseries_name.at(k),tables,true),
        reference(b.edge_bc_flow_tseries.at(k),b.edge_bc_flow_tseries_name.at(k),tables,true),
        reference(b.edge_bc_rating_curve.at(k),b.edge_bc_rating_curve_name.at(k),tables,false)})
        if(i>=0)used.push_back(i);
    std::sort(used.begin(),used.end());used.erase(std::unique(used.begin(),used.end()),used.end());
    // Preserve the existing wall/constant model bytes. Referenced tables bind
    // identity, type, records and relative-time anchoring, excluding cursors.
    if(!used.empty()) {std::string tag="authored boundary signals v1";a(tag,used);
        for(int i:used) {auto t=tables->tables.at(i);a(t.id,t.type,t.x,t.y,t.n_relative,t.rel_anchor,t.num_cols,t.column_ids);}}
    return a.bytes();
}
std::string RuntimeWaterCheckpoint::validate(const RuntimeWaterCheckpointBinding& b,
    const RuntimeSources& surface, const RuntimeSources& groundwater, const RuntimeForcings& forcing, const BoundaryData* boundary,bool completed_boundary) {
    try {
        require(&surface != &groundwater, "Runtime water checkpoint source domains must be distinct.");
        State s=privateState(b,surface,groundwater,forcing,boundary); validateState(s,b,true,completed_boundary); return {};
    } catch (const std::exception& e) { return e.what(); }
}
std::string RuntimeWaterCheckpoint::capture(const RuntimeWaterCheckpointBinding& b,
    const RuntimeSources& surface, const RuntimeSources& groundwater,
    const RuntimeForcings& forcing, std::string& bytes, const BoundaryData* boundary) {
    try {
        require(&surface != &groundwater, "Runtime water checkpoint source domains must be distinct.");
        State s=privateState(b,surface,groundwater,forcing,boundary); validateState(s,b,true);
        const bool boundaries=!b.boundary_slots.empty();Archive payload; state(payload,s,boundaries);
        std::string magic="OpenSWMM Runtime Water"; uint32_t version=boundaries?2:1;
        uint32_t endian=0x01020304, integer_size=sizeof(int), real_size=sizeof(double);
        auto identity=b.identity; auto cells=b.groundwater_cells; auto time=b.completed_time;
        auto data=payload.bytes(); auto crc=checksum(data);
        Archive record; record(magic,version,endian,integer_size,real_size,identity,cells,time);
        if(boundaries) {auto slots=b.boundary_slots;auto model=boundaryModel(*b.authored_boundary,b.boundary_tables);record(slots,model);}
        record(data,crc);
        require(record.bytes().size() <= maxBytes, "Runtime water checkpoint exceeds 64 MiB.");
        auto prepared=record.bytes(); bytes.swap(prepared); return {};
    } catch (const std::exception& e) { return e.what(); }
}
std::string RuntimeWaterCheckpoint::restore(const RuntimeWaterCheckpointBinding& b,
    std::string bytes, RuntimeSources& surface, RuntimeSources& groundwater, RuntimeForcings& forcing, BoundaryData* boundary) {
    try {
        require(&surface != &groundwater, "Runtime water checkpoint source domains must be distinct.");
        bindingOk(b); require(bytes.size() <= maxBytes, "Runtime water checkpoint exceeds 64 MiB.");
        Archive record(std::move(bytes)); std::string magic,identity,data;
        uint32_t version=0,endian=0,integer_size=0,real_size=0,crc=0;
        std::vector<bool> cells; double time=0;
        record(magic,version,endian,integer_size,real_size,identity,cells,time);
        const bool boundaries=!b.boundary_slots.empty();
        require(boundaries ? boundary && b.authored_boundary : !boundary,"Boundary checkpoint profile or target differs.");
        if(boundaries) {std::vector<bool> slots;std::string model;record(slots,model);
            require(slots==b.boundary_slots && model==boundaryModel(*b.authored_boundary,b.boundary_tables),"Boundary checkpoint edge or authored model differs.");}
        record(data,crc);record.finish();
        require(magic == "OpenSWMM Runtime Water" && version == (boundaries?2u:1u) && endian == 0x01020304 &&
            integer_size == sizeof(int) && real_size == sizeof(double),
            "Unsupported runtime water checkpoint version or platform.");
        require(identity == b.identity && cells == b.groundwater_cells && time == b.completed_time,
            "Runtime water checkpoint model, driver identity, receivers or completed time differs.");
        require(crc == checksum(data), "Runtime water checkpoint checksum differs.");
        State s;if(boundaries)s.boundary=*b.authored_boundary;
        Archive payload(std::move(data)); state(payload,s,boundaries); payload.finish(); validateState(s,b,false);
        surface=std::move(s.surface); groundwater=std::move(s.groundwater); forcing=std::move(s.forcing);
        if(boundaries)*boundary=std::move(s.boundary);
        return {};
    } catch (const std::exception& e) { return e.what(); }
}
}
