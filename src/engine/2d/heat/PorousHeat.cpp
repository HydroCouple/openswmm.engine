// SPDX-License-Identifier: Apache-2.0
#include "PorousHeat.hpp"
#include "../subsurface/SubsurfaceData.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace openswmm::twoD {
std::string ThermalMaterial::validate() const {
    if(!std::isfinite(porosity)||porosity<0||porosity>=1)return "Thermal porosity must be finite and in [0,1).";
    for(double value:{grain_density,grain_specific_heat,grain_conductivity,water_conductivity,air_conductivity,water_heat_capacity})
        if(!(value>0)||!std::isfinite(value))return "Thermal phase density, heat capacity and conductivities must be finite and positive.";
    return {};
}
double ThermalMaterial::volumetricCapacity(double theta) const {
    return (1-porosity)*grain_density*grain_specific_heat+theta*water_heat_capacity;
}
double ThermalMaterial::conductivity(double theta) const {
    return geometric?std::exp((1-porosity)*std::log(grain_conductivity)+theta*std::log(water_conductivity)+
                              (porosity-theta)*std::log(air_conductivity)):
        (1-porosity)*grain_conductivity+theta*water_conductivity+(porosity-theta)*air_conductivity;
}
namespace {
long double dot(const std::vector<double>& a,const std::vector<double>& b) {
    long double result=0;for(std::size_t i=0;i<a.size();++i)result+=static_cast<long double>(a[i])*b[i];return result;
}
}
ThermalSolution solveThermalContacts(const std::vector<ThermalStore>& stores,
    const std::vector<ThermalContact>& contacts,const std::vector<ThermalBoundary>& boundaries,double dt) {
    ThermalSolution out;
    const auto refused=[&](const char* message){out.error=message;return out;};
    if(!(dt>0)||!std::isfinite(dt))return refused("Thermal step must be finite and positive.");
    const auto n=stores.size();
    std::vector<double> diag(n),rhs(n),x(n),r(n),z(n),p(n),ap(n);
    for(std::size_t i=0;i<n;++i) {
        if(!(stores[i].capacity>0)||!std::isfinite(stores[i].capacity)||!std::isfinite(stores[i].energy))
            return refused("Thermal stores require finite energy and positive capacity.");
        diag[i]=stores[i].capacity;rhs[i]=stores[i].energy;x[i]=rhs[i]/diag[i];
    }
    for(const auto& c:contacts) {
        if(c.a<0||c.b<0||std::size_t(c.a)>=n||std::size_t(c.b)>=n||c.a==c.b||c.conductance<0||!std::isfinite(c.conductance))
            return refused("Invalid reciprocal thermal contact or conductance.");
        diag[c.a]+=dt*c.conductance;diag[c.b]+=dt*c.conductance;
    }
    for(const auto& b:boundaries) {
        if(b.store<0||std::size_t(b.store)>=n||b.conductance<0||!std::isfinite(b.conductance)||
           !std::isfinite(b.temperature)||!std::isfinite(b.power))return refused("Invalid thermal boundary.");
        diag[b.store]+=dt*b.conductance;rhs[b.store]+=dt*(b.conductance*b.temperature+b.power);
    }
    for(std::size_t i=0;i<n;++i)if(!std::isfinite(diag[i])||!std::isfinite(rhs[i]))return refused("Thermal system overflow; reduce the step or check material/forcing values.");
    const auto multiply=[&](const std::vector<double>& v,std::vector<double>& result) {
        for(std::size_t i=0;i<n;++i)result[i]=stores[i].capacity*v[i];
        // Difference form retains the exact uniform-field nullspace of the
        // contact Laplacian even for large conductance/capacity ratios.
        for(const auto& c:contacts) {
            const double q=dt*c.conductance*(v[c.a]-v[c.b]);result[c.a]+=q;result[c.b]-=q;
        }
        for(const auto& b:boundaries)result[b.store]+=dt*b.conductance*v[b.store];
    };
    multiply(x,ap);
    long double scale=0,capacity_scale=0;
    for(std::size_t i=0;i<n;++i) {
        r[i]=rhs[i]-ap[i];z[i]=r[i]/diag[i];p[i]=z[i];
        scale+=static_cast<long double>(rhs[i])*rhs[i]/diag[i];
        capacity_scale+=static_cast<long double>(stores[i].capacity)*stores[i].capacity/diag[i];
    }
    const long double tolerance=1e-26L*std::max(scale,capacity_scale); // 1e-13 in preconditioned norm
    long double rz=dot(r,z);
    const auto max_iterations=std::min<std::size_t>(4096,4*n+64);
    for(std::size_t iteration=0;rz>tolerance && iteration<max_iterations;++iteration) {
        multiply(p,ap);const long double pap=dot(p,ap);
        if(!(pap>0)||!std::isfinite(pap))return refused("Thermal implicit solve lost positive definiteness.");
        const double alpha=static_cast<double>(rz/pap);
        for(std::size_t i=0;i<n;++i){x[i]+=alpha*p[i];r[i]-=alpha*ap[i];z[i]=r[i]/diag[i];}
        long double next=dot(r,z);
        if(next<=tolerance) {
            // Check the actual equation, not just the recursive residual.
            multiply(x,ap);
            for(std::size_t i=0;i<n;++i){r[i]=rhs[i]-ap[i];z[i]=r[i]/diag[i];}
            next=dot(r,z);
        }
        const double beta=static_cast<double>(next/rz);
        for(std::size_t i=0;i<n;++i)p[i]=z[i]+beta*p[i];
        rz=next;out.iterations=int(iteration+1);
    }
    if(!std::isfinite(rz)||rz>tolerance)return refused("Thermal implicit solve did not converge; reduce the thermal step.");
    std::vector<double> energy;energy.reserve(n);for(const auto& s:stores)energy.push_back(s.energy);
    out.contact_j.reserve(contacts.size());out.boundary_j.reserve(boundaries.size());
    for(const auto& c:contacts) {
        const double joules=dt*c.conductance*(x[c.a]-x[c.b]);
        energy[c.a]-=joules;energy[c.b]+=joules;out.contact_j.push_back(joules);
    }
    for(const auto& b:boundaries) {
        const double joules=dt*(b.conductance*(b.temperature-x[b.store])+b.power);
        energy[b.store]+=joules;out.boundary_j.push_back(joules);
    }
    // Energies are updated by the accepted equal/opposite face receipts.
    // The solve residual measures their difference from C*T, in joules.
    for(std::size_t i=0;i<n;++i) {
        if(!std::isfinite(energy[i])||!std::isfinite(x[i]))return refused("Nonfinite thermal solution.");
        out.algebraic_residual=std::max(out.algebraic_residual,std::abs(energy[i]-stores[i].capacity*x[i]));
        x[i]=energy[i]/stores[i].capacity;
    }
    out.energy=std::move(energy);out.temperature=std::move(x);return out;
}
std::string PorousHeatColumn::initialize(double a,const std::vector<double>& edges,
    const ThermalMaterial& m,const std::vector<double>& water,const std::vector<double>& temperatures) {
    if(auto error=m.validate();!error.empty())return error;
    const auto n=water.size();
    if(!(a>0)||!std::isfinite(a)||!n||edges.size()!=n+1||temperatures.size()!=n||edges.front()!=0)
        return "Thermal column requires positive area and a fixed depth grid starting at zero.";
    PorousHeatColumn next;next.material=m;next.area=a;next.depth_edges=edges;next.energy.resize(n);
    for(std::size_t j=0;j<n;++j)if(!(edges[j+1]>edges[j])||!std::isfinite(edges[j+1])||!std::isfinite(temperatures[j]))
        return "Thermal layer depths must increase and initial temperatures must be finite.";
    if(auto error=next.setMoisture(water);!error.empty())return error;
    for(std::size_t j=0;j<n;++j)next.energy[j]=next.capacity[j]*temperatures[j];
    *this=std::move(next);return {};
}
std::string PorousHeatColumn::setMoisture(const std::vector<double>& water) {
    if(depth_edges.size()!=water.size()+1)return "Thermal moisture shape differs from the fixed depth grid.";
    for(double value:water)if(!std::isfinite(value)||value<0||value>material.porosity)
        return "Thermal moisture must lie between zero and porosity.";
    std::vector<double> cap(water.size()),lambda(water.size());
    for(std::size_t j=0;j<water.size();++j) {
        cap[j]=area*(depth_edges[j+1]-depth_edges[j])*material.volumetricCapacity(water[j]);
        lambda[j]=material.conductivity(water[j]);
        if(!(cap[j]>0)||!std::isfinite(cap[j])||!(lambda[j]>0)||!std::isfinite(lambda[j]))return "Invalid thermal layer capacity or conductivity.";
    }
    theta=water;capacity=std::move(cap);conductivity=std::move(lambda);return {};
}
std::vector<ThermalStore> PorousHeatColumn::stores() const {
    std::vector<ThermalStore> result;result.reserve(energy.size());
    for(std::size_t j=0;j<energy.size();++j)result.push_back({capacity[j],energy[j]});return result;
}
std::vector<ThermalContact> PorousHeatColumn::contacts() const {
    std::vector<ThermalContact> result;
    for(std::size_t j=1;j<energy.size();++j) {
        const double resistance=.5*(depth_edges[j]-depth_edges[j-1])/conductivity[j-1]+
                                .5*(depth_edges[j+1]-depth_edges[j])/conductivity[j];
        result.push_back({int(j-1),int(j),area/resistance});
    }
    return result;
}
ThermalAdvection advectPorousColumn(PorousHeatColumn& column,const std::vector<double>& moisture,
    const std::vector<double>& external_water,const std::vector<double>& external_j,const std::vector<double>& withdrawal) {
    ThermalAdvection out;
    const auto n=column.theta.size();
    if(!n || moisture.size()!=n||external_water.size()!=n||external_j.size()!=n||(!withdrawal.empty()&&withdrawal.size()!=n)) {
        out.error="Thermal advection layer shapes differ.";return out;
    }
    auto next=column;
    if(auto error=next.setMoisture(moisture);!error.empty()){out.error=error;return out;}
    std::vector<double> lower(n,0),diagonal=next.capacity,upper(n,0),rhs=column.energy,water(n+1,0);
    double volume_scale=0;
    for(std::size_t j=0;j<n;++j) {
        if(!std::isfinite(external_water[j])||!std::isfinite(external_j[j])) {out.error="Nonfinite physical water/energy receipt.";return out;}
        const double bulk=column.area*(column.depth_edges[j+1]-column.depth_edges[j]);
        const double delta=(moisture[j]-column.theta[j])*bulk;
        water[j+1]=water[j]+external_water[j]-delta;
        volume_scale+=std::abs(external_water[j])+std::abs(delta);
        rhs[j]+=external_j[j];out.external_j+=external_j[j];
    }
    out.water_residual=water.back();
    if(std::abs(out.water_residual)>1e-10*volume_scale+1e-12) {
        out.error="Accepted groundwater transfers do not close the fixed-grid water balance.";return out;
    }
    const double cp=column.material.water_heat_capacity;
    if(!withdrawal.empty())for(std::size_t j=0;j<n;++j) {
        if(withdrawal[j]<0||!std::isfinite(withdrawal[j])){out.error="Invalid accepted thermal withdrawal volume.";return out;}
        diagonal[j]+=cp*withdrawal[j];
    }
    for(std::size_t j=1;j<n;++j) {
        const double coefficient=cp*std::abs(water[j]);
        if(water[j]>=0){diagonal[j-1]+=coefficient;lower[j]-=coefficient;}
        else {diagonal[j]+=coefficient;upper[j-1]-=coefficient;}
    }
    // Conservative implicit upwind advection: a tridiagonal M-matrix,
    // distinct from the symmetric conductive/contact system above.
    for(std::size_t j=1;j<n;++j) {
        const double f=lower[j]/diagonal[j-1];diagonal[j]-=f*upper[j-1];rhs[j]-=f*rhs[j-1];
    }
    std::vector<double> temperature(n);
    out.withdrawn_j.assign(n,0);
    for(std::size_t k=n;k-->0;) {
        if(!(diagonal[k]>0)||!std::isfinite(diagonal[k])) {out.error="Invalid implicit thermal advection capacity.";return out;}
        temperature[k]=(rhs[k]-(k+1<n?upper[k]*temperature[k+1]:0))/diagonal[k];
        if(!std::isfinite(temperature[k])) {out.error="Nonfinite thermal advection solution.";return out;}
        const double withdrawn=withdrawal.empty()?0:cp*withdrawal[k]*temperature[k];
        out.withdrawn_j[k]=withdrawn;out.external_j-=withdrawn;
        next.energy[k]=column.energy[k]+external_j[k]-withdrawn;
    }
    out.vertical_water.reserve(n-1);out.vertical_j.reserve(n-1);
    for(std::size_t j=1;j<n;++j) {
        const double joules=cp*water[j]*(water[j]>=0?temperature[j-1]:temperature[j]);
        next.energy[j-1]-=joules;next.energy[j]+=joules;
        out.vertical_water.push_back(water[j]);out.vertical_j.push_back(joules);
    }
    for(double value:next.energy)if(!std::isfinite(value)){out.error="Nonfinite accepted thermal energy receipt.";return out;}
    out.temperature=std::move(temperature);
    column=std::move(next);return out;
}
std::string projectGroundMoisture(const SubsurfaceState& s,int i,double hg,const std::vector<double>& edges,
    std::vector<double>& output,std::vector<double>* saturated_water) {
    if(i<0||i>=s.n_cells||edges.size()<2)return "Invalid fixed-grid groundwater projection target.";
    const double depth=s.zs[i],porosity=s.theta_s[i],area=s.area[i];
    if(!(depth>0)||!(area>0)||!std::isfinite(hg)||hg<0||hg>depth||edges.front()!=0||
       std::abs(edges.back()-depth)>1e-12*depth)return "Thermal depth grid must span ground surface to aquifer bottom.";
    const double unsaturated=depth-hg;
    const bool sigma=static_cast<GwClosure>(s.closure[i])==GwClosure::SIGMA;
    if(sigma && s.m_layers<=0)return "SIGMA moisture projection requires accepted hydraulic layers.";
    std::vector<double> water(edges.size()-1,0),sat(water.size());
    for(std::size_t j=0;j<water.size();++j) {
        const double top=edges[j],bottom=edges[j+1],dz=bottom-top;
        if(!(dz>0)||!std::isfinite(bottom))return "Thermal depth edges must increase.";
        const double sat_depth=std::max(0.,bottom-std::max(top,unsaturated));
        double unsat_water=0;
        if(unsaturated>0) {
            if(sigma)for(int k=0;k<s.m_layers;++k) {
                const double overlap=std::max(0.,std::min(bottom,unsaturated*(k+1)/s.m_layers)-
                                                 std::max(top,unsaturated*k/s.m_layers));
                unsat_water+=overlap*s.theta_sigma[std::size_t(k)*s.n_cells+i];
            } else unsat_water=std::max(0.,std::min(bottom,unsaturated)-top)*s.hu[i]/unsaturated;
        }
        sat[j]=sat_depth*porosity*area;water[j]=(unsat_water+sat_depth*porosity)/dz;
        if(!std::isfinite(water[j])||water[j]<-1e-12||water[j]>porosity+1e-12)return "Hydraulic moisture is outside thermal phase-volume bounds.";
        water[j]=std::clamp(water[j],0.,porosity); // only accepted hydraulic roundoff, never thermal energy
    }
    output=std::move(water);if(saturated_water)*saturated_water=std::move(sat);return {};
}
} // namespace openswmm::twoD
