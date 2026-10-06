// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_RUNTIME_COUPLING_HPP
#define OPENSWMM_RUNTIME_COUPLING_HPP
#include <algorithm>
#include <array>
#include <unordered_map>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace openswmm::twoD {
// Transport rows store concentration * SI volume, including temperature-volume.
// Heat receipts use sensible energy relative to 0 C; no soil energy is implied.
inline constexpr double runtimeWaterHeatCapacity = 4186000.0; // J/(m3 K)
struct RuntimeReceipt {
    double requested = 0.0, applied = 0.0;
    double water_in = 0.0, water_out = 0.0;
    double heat_requested = 0.0, heat_applied = 0.0;
    double last_flow = 0.0, last_dt = 0.0;
    std::vector<double> requested_mass, applied_mass;
};
struct RuntimeSource {
    std::string id, owner;
    int cell = -1;
    double flow = 0.0, heat = 0.0;
    int boundary_edge = -1;
    double boundary_head = std::numeric_limits<double>::quiet_NaN();
    double until = std::numeric_limits<double>::infinity();
    bool enabled = true;
    std::vector<double> concentrations, rates;
    RuntimeReceipt receipt;
};
struct RuntimeSources {
    std::vector<RuntimeSource> rows;
    std::vector<std::vector<std::size_t>> cells;
    std::unordered_map<std::string, std::unordered_map<int, std::size_t>> index;
    RuntimeSource* find(const std::string& id, int cell) {
        const auto a=index.find(id);
        if(a==index.end()) return nullptr;
        const auto b=a->second.find(cell);
        return b==a->second.end() ? nullptr : &rows[b->second];
    }
    const RuntimeSource* find(const std::string& id, int cell) const {
        return const_cast<RuntimeSources*>(this)->find(id,cell);
    }
    void rebuild(int count) {
        cells.assign(count, {});
        for (std::size_t k=0; k<rows.size(); ++k)
            if (rows[k].enabled) cells.at(rows[k].cell).push_back(k);
    }
    void replace(RuntimeSource value) {
        if(auto* row=find(value.id,value.cell)) {
            value.receipt=std::move(row->receipt);
            *row=std::move(value); return;
        }
        index[value.id][value.cell]=rows.size();
        value.receipt.requested_mass.assign(value.concentrations.size(), 0.0);
        value.receipt.applied_mass.assign(value.concentrations.size(), 0.0);
        rows.push_back(std::move(value));
    }
    void prune() noexcept {
        for(auto& cell:cells)
            cell.erase(std::remove_if(cell.begin(),cell.end(),[this](auto k){return !rows[k].enabled;}),cell.end());
    }
    bool clear(const std::string& id, int cell=-1) noexcept {
        const auto found=index.find(id);
        if(found==index.end()) return false;
        if(cell>=0) {
            auto* row=find(id,cell); if(!row) return false; row->enabled=false;
            if(cell<static_cast<int>(cells.size())) {
                auto& active=cells[cell];
                active.erase(std::remove_if(active.begin(),active.end(),[this](auto k){return !rows[k].enabled;}),active.end());
            }
            return true;
        } else for(const auto& entry:found->second) rows[entry.second].enabled=false;
        prune(); return true;
    }
    void syncReceipts(const RuntimeSources& old) {
        for(auto& row:rows) if(const auto* previous=old.find(row.id,row.cell)) row.receipt=previous->receipt;
    }
    bool active() const { for(const auto& r:rows) if(r.enabled)return true; return false; }
    double nextTime(double now) const {
        double next=std::numeric_limits<double>::infinity();
        for(const auto& r:rows) if(r.enabled && r.until>now) next=std::min(next,r.until);
        return next;
    }
    bool expire(double now) {
        bool changed=false;
        for(auto& r:rows) if(r.enabled && r.until<=now+1e-10){ r.enabled=false; changed=true; }
        if(changed) prune();
        return changed;
    }
};
// Runtime forcing channels: rain, evaporation, paired infiltration, rain quality.
struct RuntimeForcing {
    int channel=0, cell=-1, mode=1;
    double rate=0.0, until=std::numeric_limits<double>::infinity();
    bool enabled=true;
    std::string owner;
    std::vector<double> concentrations;
    RuntimeReceipt receipt;
};
struct RuntimeForcings {
    std::vector<RuntimeForcing> rows;
    std::vector<std::array<int,4>> cells;
    std::array<int,4> uniform{{-1,-1,-1,-1}};
    std::unordered_map<int,std::unordered_map<int,std::size_t>> index;
    RuntimeForcing* find(int channel,int cell) {
        auto a=index.find(channel); if(a==index.end()) return nullptr;
        auto b=a->second.find(cell); return b==a->second.end() ? nullptr : &rows[b->second];
    }
    const RuntimeForcing* get(int channel,int cell) const noexcept {
        int k=cell>=0 && cell<static_cast<int>(cells.size()) ? cells[cell][channel-1] : -1;
        if(k<0 || !rows[k].enabled) k=uniform[channel-1];
        return k>=0 && rows[k].enabled ? &rows[k] : nullptr;
    }
    double rate(int channel,int cell,double native) const noexcept {
        const auto* row=get(channel,cell);
        return !row ? native : row->mode==2 ? native+row->rate : row->rate;
    }
    void replace(RuntimeForcing row) {
        if(auto* old=find(row.channel,row.cell)) { row.receipt=std::move(old->receipt); *old=std::move(row); }
        else { index[row.channel][row.cell]=rows.size(); rows.push_back(std::move(row)); }
    }
    void rebuild(int count) {
        cells.assign(count,{{-1,-1,-1,-1}}); uniform={{-1,-1,-1,-1}};
        for(std::size_t k=0;k<rows.size();++k) if(rows[k].enabled) {
            const auto& row=rows[k];
            if(row.cell<0) uniform[row.channel-1]=static_cast<int>(k);
            else cells.at(row.cell)[row.channel-1]=static_cast<int>(k);
        }
    }
    bool active() const noexcept { for(const auto& row:rows) if(row.enabled) return true; return false; }
    double nextTime(double now) const noexcept {
        double time=std::numeric_limits<double>::infinity();
        for(const auto& row:rows) if(row.enabled && row.until>now) time=std::min(time,row.until);
        return time;
    }
    void expire(double now) noexcept {
        for(auto& row:rows) if(row.enabled && row.until<=now+1e-10) row.enabled=false;
    }
};
} // namespace openswmm::twoD
#endif
