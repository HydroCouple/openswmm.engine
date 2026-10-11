// SPDX-License-Identifier: Apache-2.0
#include "RuntimeCouplingCheckpoint.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include <map>
#include <stdexcept>
#include <type_traits>

namespace openswmm::twoD {
namespace {
using Archive = detail::CompletedSourceArchive;
constexpr uint64_t maxBytes = 64 * 1024 * 1024;
static_assert(std::is_nothrow_move_assignable_v<RuntimeSources>);
static_assert(std::is_nothrow_move_assignable_v<RuntimeForcings>);
struct State { RuntimeSources surface, groundwater; RuntimeForcings forcing; };
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
void expiryOk(double until, bool enabled, double time) {
    require((std::isfinite(until) && until > 0) || until == std::numeric_limits<double>::infinity(),
        "Invalid runtime water checkpoint expiry.");
    require(!enabled || until > time, "Runtime water checkpoint has an active record at or past its expiry.");
}
void sourceStore(RuntimeSources& store, const RuntimeWaterCheckpointBinding& b, bool check) {
    RuntimeSources next; next.rows=store.rows;
    const int nc=static_cast<int>(b.groundwater_cells.size());
    for (std::size_t k=0; k<next.rows.size(); ++k) {
        const auto& r=next.rows[k];
        require(sourceIdOk(r) && r.cell >= 0 && r.cell < nc && std::isfinite(r.flow),
            "Invalid runtime water checkpoint source identity, cell or flow.");
        require(r.heat == 0 && r.concentrations.empty() && r.rates.empty() &&
            r.boundary_edge == -1 && std::isnan(r.boundary_head),
            "Runtime water checkpoint boundaries, heat and species sources are not qualified.");
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
void validateState(State& s, const RuntimeWaterCheckpointBinding& b, bool check) {
    bindingOk(b); sourceStore(s.surface,b,check); sourceStore(s.groundwater,b,check);
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
void state(Archive& a, State& s) {
    const auto source=[&](auto& r) { a(r.id,r.owner,r.cell,r.flow,r.until,r.enabled); receipt(a,r.receipt); };
    a.sequence(s.surface.rows,source); a.sequence(s.groundwater.rows,source);
    a.sequence(s.forcing.rows,[&](auto& r) {
        a(r.channel,r.cell,r.mode,r.rate,r.until,r.enabled,r.owner); receipt(a,r.receipt);
    });
}
}
std::string RuntimeWaterCheckpoint::validate(const RuntimeWaterCheckpointBinding& b,
    const RuntimeSources& surface, const RuntimeSources& groundwater, const RuntimeForcings& forcing) {
    try {
        require(&surface != &groundwater, "Runtime water checkpoint source domains must be distinct.");
        State s{surface,groundwater,forcing}; validateState(s,b,true); return {};
    } catch (const std::exception& e) { return e.what(); }
}
std::string RuntimeWaterCheckpoint::capture(const RuntimeWaterCheckpointBinding& b,
    const RuntimeSources& surface, const RuntimeSources& groundwater,
    const RuntimeForcings& forcing, std::string& bytes) {
    try {
        require(&surface != &groundwater, "Runtime water checkpoint source domains must be distinct.");
        State s{surface,groundwater,forcing}; validateState(s,b,true);
        Archive payload; state(payload,s);
        std::string magic="OpenSWMM Runtime Water"; uint32_t version=1;
        uint32_t endian=0x01020304, integer_size=sizeof(int), real_size=sizeof(double);
        auto identity=b.identity; auto cells=b.groundwater_cells; auto time=b.completed_time;
        auto data=payload.bytes(); auto crc=checksum(data);
        Archive record; record(magic,version,endian,integer_size,real_size,identity,cells,time,data,crc);
        require(record.bytes().size() <= maxBytes, "Runtime water checkpoint exceeds 64 MiB.");
        auto prepared=record.bytes(); bytes.swap(prepared); return {};
    } catch (const std::exception& e) { return e.what(); }
}
std::string RuntimeWaterCheckpoint::restore(const RuntimeWaterCheckpointBinding& b,
    std::string bytes, RuntimeSources& surface, RuntimeSources& groundwater, RuntimeForcings& forcing) {
    try {
        require(&surface != &groundwater, "Runtime water checkpoint source domains must be distinct.");
        bindingOk(b); require(bytes.size() <= maxBytes, "Runtime water checkpoint exceeds 64 MiB.");
        Archive record(std::move(bytes)); std::string magic,identity,data;
        uint32_t version=0,endian=0,integer_size=0,real_size=0,crc=0;
        std::vector<bool> cells; double time=0;
        record(magic,version,endian,integer_size,real_size,identity,cells,time,data,crc); record.finish();
        require(magic == "OpenSWMM Runtime Water" && version == 1 && endian == 0x01020304 &&
            integer_size == sizeof(int) && real_size == sizeof(double),
            "Unsupported runtime water checkpoint version or platform.");
        require(identity == b.identity && cells == b.groundwater_cells && time == b.completed_time,
            "Runtime water checkpoint model, driver identity, receivers or completed time differs.");
        require(crc == checksum(data), "Runtime water checkpoint checksum differs.");
        State s; Archive payload(std::move(data)); state(payload,s); payload.finish(); validateState(s,b,false);
        surface=std::move(s.surface); groundwater=std::move(s.groundwater); forcing=std::move(s.forcing);
        return {};
    } catch (const std::exception& e) { return e.what(); }
}
}
