// SPDX-License-Identifier: Apache-2.0
#include "PorewaterTransport.hpp"
#include <algorithm>
#include <cmath>
namespace openswmm::twoD {
PorewaterAdvection advectPorewater(const std::vector<double>& old_water,
    const std::vector<double>& new_water,const std::vector<double>& retention,
    const std::vector<double>& mass,const std::vector<double>& external_water,
    const std::vector<double>& external_mass,const std::vector<double>& withdrawal) {
    const auto refused=[](const char* message){PorewaterAdvection out;out.error=message;return out;};
    const auto n=mass.size();
    if(!n||old_water.size()!=n||new_water.size()!=n||retention.size()!=n||external_water.size()!=n||
       external_mass.size()!=n||withdrawal.size()!=n)return refused("Porewater transport layer shapes differ.");
    std::vector<double> capacity(n),diagonal(n),lower(n,0),upper(n,0),rhs(n),water(n+1,0);
    double volume_scale=0;
    for(std::size_t j=0;j<n;++j) {
        for(double value:{old_water[j],new_water[j],retention[j],mass[j],withdrawal[j]})
            if(!std::isfinite(value)||value<0)return refused("Porewater water, retention, mass and withdrawals must be finite and nonnegative.");
        if(!std::isfinite(external_water[j])||!std::isfinite(external_mass[j]))return refused("Nonfinite accepted porewater receipt.");
        capacity[j]=new_water[j]+retention[j];diagonal[j]=capacity[j]+withdrawal[j];rhs[j]=mass[j]+external_mass[j];
        if(rhs[j]<0)return refused("Accepted porewater export exceeds its physical layer donor mass.");
        const double delta=new_water[j]-old_water[j];
        water[j+1]=water[j]+external_water[j]-delta;volume_scale+=std::abs(external_water[j])+std::abs(delta);
    }
    if(!std::isfinite(volume_scale)||!std::isfinite(water.back())||std::abs(water.back())>1e-10*volume_scale+1e-12)
        return refused("Accepted porewater transfers do not close the fixed-layer water balance.");
    for(std::size_t j=1;j<n;++j) {
        const double q=std::abs(water[j]);
        if(water[j]>=0){diagonal[j-1]+=q;lower[j]-=q;}
        else {diagonal[j]+=q;upper[j-1]-=q;}
    }
    std::vector<unsigned char> residue(n,0);
    for(std::size_t j=0;j<n;++j) {
        if(!std::isfinite(diagonal[j])||!std::isfinite(rhs[j]))return refused("Porewater transport system overflow.");
        if(diagonal[j]==0) {
            // No water/sorption capacity and no outgoing dissolved packet.
            // Incoming solute accumulates as immobile dry residue. This row
            // cannot supply another row, so remove only its own incoming
            // equation; retain those receipts in the conservative reconstruction.
            residue[j]=1;diagonal[j]=1;rhs[j]=0;lower[j]=upper[j]=0;
        }
    }
    for(std::size_t j=1;j<n;++j) {
        if(!(diagonal[j-1]>0)||!std::isfinite(diagonal[j-1]))return refused("Invalid implicit porewater donor capacity.");
        const double f=lower[j]/diagonal[j-1];diagonal[j]-=f*upper[j-1];rhs[j]-=f*rhs[j-1];
    }
    std::vector<double> concentration(n),out_mass(n),withdrawn(n),vertical_mass(n-1);
    for(std::size_t j=n;j-->0;) {
        if(!(diagonal[j]>0)||!std::isfinite(diagonal[j]))return refused("Invalid implicit porewater donor capacity.");
        concentration[j]=(rhs[j]-(j+1<n?upper[j]*concentration[j+1]:0))/diagonal[j];
        if(!std::isfinite(concentration[j])||concentration[j]<0)return refused("Nonfinite or negative porewater transport solution.");
        withdrawn[j]=withdrawal[j]*concentration[j];
        // C*c is the canonical equilibrated mass. A zero final capacity
        // after throughflow holds exactly zero, without a fake pore volume.
        out_mass[j]=capacity[j]*concentration[j];
    }
    for(std::size_t j=1;j<n;++j)vertical_mass[j-1]=water[j]*(water[j]>=0?concentration[j-1]:concentration[j]);
    double external_total=0;
    for(std::size_t j=0;j<n;++j) {
        external_total+=external_mass[j]-withdrawn[j];
        if(residue[j]) {
            long double retained=static_cast<long double>(mass[j])+external_mass[j]-withdrawn[j];
            if(j>0)retained+=vertical_mass[j-1];if(j+1<n)retained-=vertical_mass[j];
            out_mass[j]=double(retained);
        }
        if(!std::isfinite(out_mass[j])||out_mass[j]<0||!std::isfinite(withdrawn[j]))return refused("Nonfinite or negative accepted porewater mass.");
    }
    if(!std::isfinite(external_total))return refused("Porewater transport receipt overflow.");
    PorewaterAdvection out;out.mass=std::move(out_mass);out.concentration=std::move(concentration);
    out.withdrawn_mass=std::move(withdrawn);out.vertical_mass=std::move(vertical_mass);
    out.vertical_water.assign(water.begin()+1,water.end()-1);out.external_mass=external_total;out.water_residual=water.back();return out;
}
} // namespace openswmm::twoD
