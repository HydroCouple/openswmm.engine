// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "PassiveStorageLegAdmission.hpp"
#include <cmath>
#include <limits>
#include <algorithm>
namespace openswmm::quality {
PassiveStorageExchange::AppliedLeg passiveStorageAttempt(std::size_t point, int cell, int node,
        double water, double mass, double before, double volume, double offset, double duration,
        double withdrawal, double factor, double start, double end, PassiveStorageExchange::Phase phase) noexcept {
    PassiveStorageExchange::AppliedLeg l{point,cell,node,water>0,false,std::abs(water),mass,
        std::abs(water)*factor,mass*factor,before,water>0?before-mass:before+mass,0,start,end,phase};
    l.volume_before=volume;l.volume_after=std::max(0.0,volume-water);
    l.offset_s=offset;l.duration_s=duration;l.withdrawal_si=withdrawal;return l;
}
const char* passiveStorageLegError(const PassiveStorageExchange::AppliedLeg& l, double water) noexcept {
    const double mass=l.mass_si,before=l.before,volume=l.volume_before,offset=l.offset_s,duration=l.duration_s,withdrawal=l.withdrawal_si;
    if (!std::isfinite(water) || water==0 || !std::isfinite(mass) || mass<0 ||
        !std::isfinite(before) || before<0 || !std::isfinite(l.after) || l.after<0 ||
        !std::isfinite(l.water_native) || l.water_native==0 ||
        !std::isfinite(l.mass_native) || (mass!=0 && l.mass_native==0))
        return "Live passive exchange leg is nonfinite or has no representable carrier.";
    if (!std::isfinite(volume) || volume<0 || (water>0 && volume==0) || !std::isfinite(l.volume_after) ||
        !std::isfinite(offset) || offset<0 || !std::isfinite(duration) || duration<=0 ||
        offset+duration>(l.batch_end_ms-l.batch_start_ms)/1000)
        return "Live passive exchange donor water or firing interval differs.";
    if (water>0 && mass!=std::min(water*(before/volume),before))
        return "Live passive exchange drain differs from its actual donor operands.";
    if (!std::isfinite(withdrawal) || withdrawal<0 || (water>0?withdrawal!=0:(withdrawal/duration)*duration!=-water))
        return "Live passive exchange water withdrawal differs from native operands.";
    const double delta=water>0?-mass:mass;
    const double bound=2*std::numeric_limits<double>::epsilon()*std::abs(l.after);
    if (mass!=0 && (l.after==before || std::abs((l.after-before)-delta)>bound))
        return "Live passive exchange source increment is not representable.";
    if (water<0 && mass!=l.water_si*l.sample)
        return "Live passive exchange spill differs from its actual SI sample.";
    return nullptr;
}
}
