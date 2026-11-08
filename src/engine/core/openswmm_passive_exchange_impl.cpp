// SPDX-License-Identifier: Apache-2.0
#include <openswmm/engine/openswmm_passive_exchange.h>
#include "SWMMEngine.hpp"
#ifdef OPENSWMM_HAS_2D
#include "../quality/LivePassiveStorageExchange.hpp"
#endif
#include <algorithm>
#include <cstring>
#include <exception>
#include <new>
#include <string_view>

namespace {
thread_local char diagnostic[2048]{};
int failure(int code, std::string_view text) noexcept {
    auto n = std::min(text.size(), sizeof diagnostic - 1);
    if (n < text.size()) while (n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) --n;
    std::memcpy(diagnostic, text.data(), n); diagnostic[n] = 0; return code;
}
template<class F> int operation(F&& call) noexcept {
    diagnostic[0] = 0;
    try { return call(); }
    catch (const std::bad_alloc&) { return failure(SWMM_ERR_NOMEM, "Cannot allocate passive exchange operation."); }
    catch (const std::exception& e) { return failure(SWMM_ERR_INTERNAL, e.what()); }
    catch (...) { return failure(SWMM_ERR_INTERNAL, "Non-standard passive exchange exception."); }
}
using Engine = openswmm::SWMMEngine;
#ifdef OPENSWMM_HAS_2D
using Owner = openswmm::quality::LivePassiveStorageExchange;
int owner(SWMM_Engine handle, const Owner*& out) {
    auto& e = *static_cast<Engine*>(handle);
    if (!e.passiveExchangeInspectionReady()) return failure(SWMM_ERR_LIFECYCLE, "Passive exchange inspection requires idle engine access.");
    const auto* o = e.livePassiveExchange();
    if (!o) return failure(SWMM_ERR_LIFECYCLE, "No passive exchange owner is enrolled.");
    if (!o->receipts().changingVolume() || !o->receipts().exactSlices() || !o->receipts().retainsWaterAggregates())
        return failure(SWMM_ERR_PLUGIN, "This facade requires the changing-volume exact-slice profile.");
    out = o; return SWMM_OK;
}
SWMM_PassiveAmountV1 amount(const openswmm::quality::NativeAmountSum& value) {
    static_assert(openswmm::quality::NativeAmountSum::capacity == SWMM_PASSIVE_EXCHANGE_COMPONENTS);
    SWMM_PassiveAmountV1 out{}; out.count = value.parts().size();
    std::copy(value.parts().begin(), value.parts().end(), out.components); return out;
}
SWMM_PassiveLegV1 leg(const Owner::Leg& l) {
    SWMM_PassiveLegV1 out{};
    out.point=l.point; out.cell=l.cell; out.node=l.node; out.drain=l.drain; out.applied=l.applied; out.phase=int(l.phase);
    out.water_m3=l.water_si; out.source_amount=l.mass_si; out.water_ft3=l.water_native; out.native_amount=l.mass_native;
    out.source_before=l.before; out.source_after=l.after; out.sampled_concentration=l.sample;
    out.volume_before_m3=l.volume_before; out.volume_after_m3=l.volume_after;
    out.start_ms=l.batch_start_ms; out.end_ms=l.batch_end_ms; out.offset_seconds=l.offset_s; out.duration_seconds=l.duration_s;
    return out;
}
#endif
template<class T, class F> int read(SWMM_Engine handle, size_t size, T* out, F&& call) {
    return operation([&]() -> int {
        if (!handle) return failure(SWMM_ERR_BADHANDLE, "Null passive exchange engine.");
        if (!out || size != sizeof(T)) return failure(SWMM_ERR_BADPARAM, "Invalid version1 passive exchange output/size.");
#ifdef OPENSWMM_HAS_2D
        const Owner* o = nullptr; const int ready = owner(handle, o); if (ready) return ready;
        return call(*static_cast<Engine*>(handle), *o, out);
#else
        return failure(SWMM_ERR_PLUGIN, "Passive exchange unavailable without 2D.");
#endif
    });
}
}

extern "C" {
const char* swmm_passive_exchange_last_error(void) { return diagnostic; }
int swmm_passive_exchange_enable(SWMM_Engine handle, uint64_t capacity, int preflight) {
    return operation([&]() -> int {
        if (!handle) return failure(SWMM_ERR_BADHANDLE, "Null passive exchange engine.");
        if (!capacity || capacity > 65536 || (preflight != 0 && preflight != 1))
            return failure(SWMM_ERR_BADPARAM, "Capacity must be 1..65536 and preflight 0 or 1.");
#ifdef OPENSWMM_HAS_2D
        auto& e = *static_cast<Engine*>(handle);
        if (!e.passiveExchangeInspectionReady() || e.context().state != openswmm::EngineState::RUNNING ||
            e.context().elapsed_ms != 0 || e.livePassiveExchange())
            return failure(SWMM_ERR_LIFECYCLE, "Enroll at the initial running boundary with no prior owner.");
        const auto error = e.attachLiveChangingVolumeSlicedExchange(static_cast<size_t>(capacity), preflight != 0);
        return error.empty() ? SWMM_OK : failure(SWMM_ERR_PLUGIN, error);
#else
        return failure(SWMM_ERR_PLUGIN, "Passive exchange unavailable without 2D.");
#endif
    });
}
int swmm_passive_exchange_enable_checkpoint(SWMM_Engine handle) {
    return operation([&]() -> int {
        if (!handle) return failure(SWMM_ERR_BADHANDLE, "Null passive exchange engine.");
#ifdef OPENSWMM_HAS_2D
        const Owner* o=nullptr; const int ready=owner(handle,o); if (ready) return ready;
        auto& e=*static_cast<Engine*>(handle);
        if (e.hotstartCheckpointAttached()) return failure(SWMM_ERR_LIFECYCLE, "Checkpoint already attached.");
        const auto error=e.enableCheckpointing();
        return error.empty() ? SWMM_OK : failure(SWMM_ERR_LIFECYCLE,error);
#else
        return failure(SWMM_ERR_PLUGIN, "Passive exchange unavailable without 2D.");
#endif
    });
}
int swmm_passive_exchange_status_v1(SWMM_Engine h,size_t n,SWMM_PassiveStatusV1* out) {
    return read(h,n,out,[](const auto& e,const auto& o,auto* output) -> int {
        SWMM_PassiveStatusV1 v{}; const auto& b=o.receipts();
        v.receipt_count=b.receipts().size(); v.leg_count=o.legCount(); v.unsealed_leg_count=v.leg_count;
        for (const auto& r:b.receipts()) v.unsealed_leg_count-=r.proof.legs.size();
        v.pollutant=0; v.units=int(e.context().pollutants.units[0]); v.failed=o.failed();
        v.window_open=o.windowOpen(); v.delivery_frozen=b.deliveryFrozen(); v.history_preflight=o.preflightsHistory();
        v.checkpoint_attached=e.hotstartCheckpointAttached(); v.history_refused=!o.historyPreflightError().empty(); v.elapsed_ms=e.context().elapsed_ms;
        v.delivery_seconds=e.context().coupling_delivery_remaining; *output=v; return SWMM_OK;
    });
}
int swmm_passive_exchange_pool_v1(SWMM_Engine h,int node,size_t n,SWMM_PassivePoolV1* out) {
    return read(h,n,out,[&](const auto& e,const auto& o,auto* output) -> int {
        const auto& b=o.receipts(); const auto* p=b.settledPool(node);
        if (!p) return failure(SWMM_ERR_BADINDEX,"Unknown enrolled pool.");
        SWMM_PassivePoolV1 v{}; v.node=node; v.units=int(e.context().pollutants.units[0]);
        v.settled_volume_ft3=p->volume; v.settled_depth_ft=p->depth; v.settled_concentration=p->concentration;
        v.settled_physical_amount=p->volume*p->concentration; v.hydraulic_volume_ft3=e.context().nodes.volume[node];
        v.roundoff=amount(b.storedRoundoff(node)); v.pending_drain=amount(b.pendingDrainAmount(node)); v.pending_spill=amount(b.pendingSpillAmount(node));
        *output=v; return SWMM_OK;
    });
}
int swmm_passive_exchange_receipt_v1(SWMM_Engine h,uint64_t index,size_t n,SWMM_PassiveReceiptV1* out) {
    return read(h,n,out,[&](const auto&,const auto& o,auto* output) -> int {
        const auto& rs=o.receipts().receipts(); if (index>=rs.size()) return failure(SWMM_ERR_BADINDEX,"Unknown receipt ordinal.");
        const auto& r=rs[static_cast<size_t>(index)]; const auto& p=r.publication; SWMM_PassiveReceiptV1 v{};
        for (size_t i=0;i<index;++i) v.first_leg+=rs[i].proof.legs.size();
        v.id=r.id; v.row_count=r.accepted.size(); v.leg_count=r.proof.legs.size(); v.generation=p.generation; v.phase=int(p.phase);
        v.capture_ms=p.capture_ms; v.start_ms=p.batch_start_ms; v.end_ms=p.batch_end_ms; v.eligible_ms=p.eligible_ms;
        v.publication_delivery_seconds=p.remaining; *output=v; return SWMM_OK;
    });
}
int swmm_passive_exchange_row_v1(SWMM_Engine h,uint64_t index,int remaining,uint64_t row,size_t n,SWMM_PassiveRowV1* out) {
    return read(h,n,out,[&](const auto&,const auto& o,auto* output) -> int {
        if (remaining!=0 && remaining!=1) return failure(SWMM_ERR_BADPARAM,"remaining must be 0 or 1.");
        const auto& rs=o.receipts().receipts(); if (index>=rs.size()) return failure(SWMM_ERR_BADINDEX,"Unknown receipt ordinal.");
        const auto& rows=remaining?rs[static_cast<size_t>(index)].remaining:rs[static_cast<size_t>(index)].accepted;
        if (row>=rows.size()) return failure(SWMM_ERR_BADINDEX,"Unknown row ordinal.");
        const auto& r=rows[static_cast<size_t>(row)]; SWMM_PassiveRowV1 v{}; v.point=r.point; v.node=r.node; v.units=int(r.units);
        v.drain_water=amount(r.waterAmount(true)); v.spill_water=amount(r.waterAmount(false));
        v.drain_amount=amount(r.materialAmount(true)); v.spill_amount=amount(r.materialAmount(false)); *output=v; return SWMM_OK;
    });
}
int swmm_passive_exchange_leg_v1(SWMM_Engine h,uint64_t index,size_t n,SWMM_PassiveLegV1* out) {
    return read(h,n,out,[&](const auto&,const auto& o,auto* output) -> int {
        if (index>=o.legCount()) return failure(SWMM_ERR_BADINDEX,"Unknown journal leg ordinal.");
        auto v=leg(o.leg(static_cast<size_t>(index))); auto relative=index;
        for (const auto& r:o.receipts().receipts()) {
            if (relative<r.proof.legs.size()) {
                v.sealed=1; v.remaining_water_ft3=r.remaining_water_legs[static_cast<size_t>(relative)];
                v.remaining_native_amount=r.remaining_mass_legs[static_cast<size_t>(relative)]; break;
            }
            relative-=r.proof.legs.size();
        }
        *output=v; return SWMM_OK;
    });
}
int swmm_passive_exchange_attempt_v1(SWMM_Engine h,size_t n,SWMM_PassiveLegV1* out) {
    return read(h,n,out,[](const auto&,const auto& o,auto* output) -> int { *output=leg(o.attempted()); return SWMM_OK; });
}
}
