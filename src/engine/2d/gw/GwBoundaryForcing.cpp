// SPDX-License-Identifier: Apache-2.0
#include "GwBoundaryForcing.hpp"
#include "GwSourceResolver.hpp"
#include "GwTransportData.hpp"
#include "../data/MeshData.hpp"
#include "../subsurface/SubsurfaceSections.hpp"
#include "../subsurface/SubsurfaceTransportState.hpp"
#include "../../core/SimulationContext.hpp"
#include <cctype>
#include <set>
#include <stdexcept>
#include <unordered_map>
namespace openswmm::twoD {
namespace {
[[noreturn]] void bad(const std::string& text) {
    throw std::runtime_error("[2D_AQUIFER_BOUNDARIES]/[GW_BOUNDARY_QUALITY] "+text);
}
GwSourceSignal scaled(GwSourceSignal signal,double scale) {
    signal.constant*=scale;
    if(signal.series) {
        auto series=std::make_shared<GwSourceSeries>(*signal.series);
        for(auto& v:series->values)v*=scale;
        signal.series=std::move(series);
    } else for(auto& v:signal.values)v*=scale;
    return signal;
}
void knots(const GwSourceSignal& signal,double start,double end,std::vector<double>& out) {
    const auto& t=signal.timePoints();
    for(auto i=std::upper_bound(t.begin(),t.end(),start);i!=t.end()&&*i<end;++i)out.push_back(*i);
}
}
std::vector<GwResolvedBoundary> resolveGwBoundaries(SimulationContext& ctx,const MeshData& mesh,
    const SubsurfaceConfig& config,const GwTransportData& quality,const SubsurfaceState& state,
    const SubsurfaceTransportState& tr,const std::vector<uint8_t>& coverage,double heat_capacity) {
    if(config.options.per_subcatch&&!config.boundaries.empty())bad("hydraulic edges require MODE MESH");
    std::unordered_map<std::string,GwSourceSignal> cache;
    std::vector<GwResolvedBoundary> result;
    const auto units=gwUnitFactors(ctx);
    std::set<std::pair<int,int>> seen;
    for(const auto& authored:config.boundaries) {
        const int cell=authored.cell,edge=authored.edge;
        if(cell<0||cell>=state.n_cells||edge<0||edge>=mesh.cell_nv[cell])bad("invalid cell/local edge");
        const int slot=MeshData::slot(cell,edge);
        if(mesh.cell_nbr[slot]!=-1)bad("hydraulic boundary must be an exterior mesh edge");
        if(!coverage[cell])bad("boundary cell has no active aquifer");
        if(!seen.insert({cell,edge}).second)bad("duplicate hydraulic boundary edge");
        GwResolvedBoundary b;b.cell=cell;b.edge=edge;b.head=authored.head;
        b.length=mesh.edge_length[slot];b.distance=mesh.edge_dist_c[slot];
        if(!std::isfinite(b.length)||!(b.length>0)||!std::isfinite(b.distance)||!(b.distance>0))bad("invalid edge geometry");
        b.signal=resolveGwSignal(ctx,authored.value,authored.series,cache,"[2D_AQUIFER_BOUNDARIES]");
        if(b.head)b.signal=scaled(std::move(b.signal),units.length);
        b.mass_in.assign(tr.n_species,0);b.mass_out=b.mass_in;b.flux_in=b.mass_in;b.flux_out=b.mass_in;
        std::set<std::pair<int,bool>> term_seen;
        std::set<int> concentrations;
        for(const auto& row:quality.boundary_quality)if(row.cell==cell&&row.edge==edge) {
            GwBoundaryTerm term;term.row=tr.rowIndex(row.species);
            if(term.row<0)bad("boundary species '"+row.species+"' is not enabled in groundwater");

            term.flux=row.kind=="MASSFLUX"||row.kind=="HEATFLUX";
            if(!term_seen.insert({term.row,term.flux}).second)bad("duplicate species boundary term");
            term.signal=resolveGwSignal(ctx,row.value,row.ts_name,cache,"[GW_BOUNDARY_QUALITY]");
            if(term.flux) {
                if(row.kind=="HEATFLUX") {
                    if(term.row!=tr.temp_row)bad("HEATFLUX requires __TEMPERATURE__");
                    if(!std::isfinite(heat_capacity)||!(heat_capacity>0))bad("invalid water heat capacity");
                    term.scale=b.length/heat_capacity;
                } else {
                    if(term.row==tr.age_row||term.row==tr.temp_row)bad("MASSFLUX requires a mass/count species");
                    if(term.row>=tr.n_pollut) {
                        const int m=ctx.reactions.find_species(tr.row_names[term.row]);
                        if(m<0||m>=ctx.reactions.n_species()||ctx.reactions.species_is_wall[m])bad("MASSFLUX needs a dissolved species");
                        std::string unit=ctx.reactions.species_units[m];
                        for(auto& c:unit)c=char(std::toupper(static_cast<unsigned char>(c)));
                        if(unit.size()>2&&unit.ends_with("/L"))unit.resize(unit.size()-2);
                        const std::set<std::string> known{"MG","UG","G","KG","MOL","MMOL","UMOL","COUNT","#"};
                        if(!known.contains(unit))bad("unresolved native amount basis for species '"+row.species+"'");
                    }
                    term.scale=.001*b.length; // native amount/s/m -> native concentration*m3
                }
            } else {
                concentrations.insert(term.row);
                if(term.row!=tr.temp_row && (term.signal.constant<0 ||
                    (term.signal.series&&term.signal.series->hasNegative)))bad("negative boundary concentration or age");
            }
            b.terms.push_back(std::move(term));
        }
        const auto& values=b.signal.ordinates();
        const bool may_inflow=b.head || b.signal.constant>0 ||
            std::any_of(values.begin(),values.end(),[](double x){return x>0;});
        if(may_inflow)for(int r=0;r<tr.n_species;++r)
            if(!concentrations.contains(r))bad("inflow requires explicit composition for '"+tr.row_names[r]+"'");
        result.push_back(std::move(b));
    }
    for(const auto& row:quality.boundary_quality)
        if(!seen.contains({row.cell,row.edge}))bad("quality has no matching hydraulic boundary edge");
    return result;
}
void appendGwBoundaryCuts(const GwResolvedBoundary& b,double start,double end,double head,
    double bottom,double thickness,std::vector<double>& cuts) {
    knots(b.signal,start,end,cuts);
    for(const auto& term:b.terms) {
        knots(term.signal,start,end,cuts);
        if(term.flux) {
            std::vector<double> times{start,end};knots(term.signal,start,end,times);std::sort(times.begin(),times.end());
            for(std::size_t i=1;i<times.size();++i) {
                const double a=term.signal.at(times[i-1]),z=term.signal.at(times[i]);
                if((a<0&&z>0)||(a>0&&z<0))cuts.push_back(times[i-1]+(times[i]-times[i-1])*(-a)/(z-a));
            }
        }
    }
    std::vector<double> t{start,end};knots(b.signal,start,end,t);std::sort(t.begin(),t.end());
    const std::vector<double> levels=b.head?std::vector<double>{head,bottom,bottom+thickness}:std::vector<double>{0};
    for(std::size_t i=1;i<t.size();++i) {
        const double a=b.signal.at(t[i-1]),z=b.signal.at(t[i]);
        if(a==z)continue;
        for(double level:levels)if((a<level&&z>level)||(a>level&&z<level))
            cuts.push_back(t[i-1]+(t[i]-t[i-1])*(level-a)/(z-a));
    }
}
GwBoundaryPulse integrateGwBoundary(const GwResolvedBoundary& b,double start,double dt,
    double head,double bottom,double thickness,double conductivity,int species) {
    GwBoundaryPulse pulse;pulse.mass.assign(species,0);pulse.flux_in.assign(species,0);pulse.flux_out.assign(species,0);
    std::vector<double> t{start,start+dt};appendGwBoundaryCuts(b,start,start+dt,head,bottom,thickness,t);
    std::sort(t.begin(),t.end());t.erase(std::unique(t.begin(),t.end()),t.end());
    for(std::size_t i=1;i<t.size();++i) {
        const double middle=(t[i]+t[i-1])/2,half=(t[i]-t[i-1])/2;
        // Two-point Gauss is exact for Q*C on each linear forcing interval:
        // a held-cell unconfined head boundary has quadratic Q in head.
        for(double sign:{-1.,1.}) {
            const double time=middle+sign*half/std::sqrt(3.);
            double q=b.signal.at(time);
            if(b.head) {
                const double outside=std::max(q,bottom);
                const double h=std::clamp(outside-bottom,0.,thickness);
                q=conductivity*.5*(head-bottom+h)*b.length/b.distance*(outside-head);
            }
            const double volume=q*half;
            if(volume>0)pulse.in+=volume;else pulse.out-=volume;
            for(const auto& term:b.terms) {
                const double value=term.signal.at(time);
                if(term.flux) {
                    const double amount=half*value*term.scale;
                    if(amount>=0)pulse.flux_in[term.row]+=amount;else pulse.flux_out[term.row]-=amount;
                }
                else if(volume>0)pulse.mass[term.row]+=volume*value;
            }
        }
    }
    if(!std::isfinite(pulse.in)||!std::isfinite(pulse.out))bad("nonfinite integrated edge water forcing");
    for(const auto* values:{&pulse.mass,&pulse.flux_in,&pulse.flux_out})for(double value:*values)
        if(!std::isfinite(value))bad("nonfinite integrated edge species forcing");
    return pulse;
}
}
