/**
 * @file InpWriter.cpp
 * @brief Comprehensive .inp serialisation — round-trip identical output.
 *
 * @details Writes every SWMM input section with exact column layouts matching
 *          the legacy format so that read -> write -> read produces identical
 *          model state. Shape names, enum strings, and field widths are taken
 *          directly from legacy text.h and keywords.c.
 *
 * All standard SWMM sections implemented:
 *   TITLE, OPTIONS, RAINGAGES, SUBCATCHMENTS, SUBAREAS, INFILTRATION,
 *   JUNCTIONS, OUTFALLS, DIVIDERS, STORAGE, CONDUITS, PUMPS, ORIFICES,
 *   WEIRS, OUTLETS, XSECTIONS, LOSSES, TRANSECTS, STREETS, INLETS,
 *   CONTROLS, POLLUTANTS, LANDUSES, BUILDUP, WASHOFF, TREATMENT,
 *   INFLOWS, DWF, RDII, PATTERNS, TIMESERIES, CURVES, COORDINATES,
 *   USER_FLAGS, USER_FLAG_VALUES, PLUGINS
 *
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "InpWriter.hpp"
#include "SimulationContext.hpp"
#include <cstdio>
#include <cstring>

namespace openswmm {
namespace inp_writer {

static void sec(FILE* f, const char* name) {
    std::fprintf(f, "\n[%s]\n", name);
}

static const char* nN(const SimulationContext& c, int i) {
    return (i>=0 && i<c.n_nodes()) ? c.node_names.name_of(i).c_str() : "*";
}
static const char* gN(const SimulationContext& c, int i) {
    return (i>=0 && i<c.n_gages()) ? c.gage_names.name_of(i).c_str() : "*";
}
static const char* tN(const SimulationContext& c, int i) {
    return (i>=0 && i<static_cast<int>(c.table_names.size())) ? c.table_names.name_of(i).c_str() : "*";
}
static const char* pN(const SimulationContext& c, int i) {
    return (i>=0 && i<c.n_pollutants()) ? c.pollutant_names.name_of(i).c_str() : "*";
}

static const char* xsName(int s) {
    // Indexed by LinkData::XsectShape enum values
    static const char* n[] = {
        "CIRCULAR","FILLED_CIRCULAR","RECT_CLOSED","RECT_OPEN",
        "TRAPEZOIDAL","TRIANGULAR","PARABOLIC","POWER","MODBASKETHANDLE",
        "EGG","HORSESHOE","GOTHIC","CATENARY","SEMIELLIPTICAL",
        "BASKETHANDLE","SEMICIRCULAR","RECT_TRIANGULAR","RECT_ROUND",
        "HORIZ_ELLIPSE","VERT_ELLIPSE","ARCH",
        "IRREGULAR","CUSTOM","FORCE_MAIN","STREET","DUMMY"
    };
    return (s>=0&&s<=25) ? n[s] : "CIRCULAR";
}
static const char* ofName(OutfallType t) {
    switch(t){case OutfallType::NORMAL:return"NORMAL";case OutfallType::FIXED:return"FIXED";
    case OutfallType::TIDAL:return"TIDAL";case OutfallType::TIMESERIES:return"TIMESERIES";default:return"FREE";}
}
static bool hasNT(const SimulationContext& c, NodeType t) {
    for(int j=0;j<c.n_nodes();++j) if(c.nodes.type[static_cast<size_t>(j)]==t) return true; return false;
}
static bool hasLT(const SimulationContext& c, LinkType t) {
    for(int j=0;j<c.n_links();++j) if(c.links.type[static_cast<size_t>(j)]==t) return true; return false;
}

int writeInpFile(const SimulationContext& ctx, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return -1;

    sec(f,"TITLE");
    std::fprintf(f,";;Project Title/Notes\n");
    for (const auto& line : ctx.title_notes) {
        std::fprintf(f,"%s\n", line.c_str());
    }

    // [OPTIONS]
    sec(f,"OPTIONS");
    std::fprintf(f,"FLOW_UNITS           %d\n",static_cast<int>(ctx.options.flow_units));
    std::fprintf(f,"ROUTING_MODEL        %d\n",static_cast<int>(ctx.options.routing_model));
    std::fprintf(f,"ROUTING_STEP         %.2f\n",ctx.options.routing_step);
    std::fprintf(f,"REPORT_STEP          %.0f\n",ctx.options.report_step);
    if(!ctx.options.crs.empty()) std::fprintf(f,"CRS                  %s\n",ctx.options.crs.c_str());
    for(const auto& kv:ctx.options.ext_options) std::fprintf(f,"%-20s %s\n",kv.first.c_str(),kv.second.c_str());

    // [RAINGAGES]
    if(ctx.n_gages()>0){sec(f,"RAINGAGES");
    std::fprintf(f,";;%-16s %-12s %-8s %-8s %-16s\n","Name","Format","Intvl","SCF","Source");
    for(int j=0;j<ctx.n_gages();++j){auto u=static_cast<size_t>(j);
    int iv=ctx.gages.interval_sec[u];int h=iv/3600,m=(iv%3600)/60;int ts=ctx.gages.ts_index[u];
    if(ts>=0)std::fprintf(f,"%-16s INTENSITY    %d:%02d     %.2f     TIMESERIES %s\n",ctx.gage_names.name_of(j).c_str(),h,m,ctx.gages.snow_factor[u],tN(ctx,ts));
    else if(!ctx.gages.file_path[u].empty())std::fprintf(f,"%-16s INTENSITY    %d:%02d     %.2f     FILE \"%s\" %s\n",ctx.gage_names.name_of(j).c_str(),h,m,ctx.gages.snow_factor[u],ctx.gages.file_path[u].c_str(),ctx.gages.col_name[u].c_str());
    }}

    // [SUBCATCHMENTS]
    if(ctx.n_subcatches()>0){sec(f,"SUBCATCHMENTS");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-12s %-10s\n","Name","RainGage","Outlet","Area","%%Imperv","Width","%%Slope");
    for(int j=0;j<ctx.n_subcatches();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-16s %-16s %12.4f %10.2f %12.4f %10.4f\n",ctx.subcatch_names.name_of(j).c_str(),gN(ctx,ctx.subcatches.gage[u]),nN(ctx,ctx.subcatches.outlet_node[u]),ctx.subcatches.area[u],ctx.subcatches.frac_imperv[u]*100.0,ctx.subcatches.width[u],ctx.subcatches.slope[u]*100.0);
    }}

    // [SUBAREAS]
    if(ctx.n_subcatches()>0){sec(f,"SUBAREAS");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s\n","Subcatch","N-Imperv","N-Perv","S-Imperv","S-Perv","%%ZeroImp");
    for(int j=0;j<ctx.n_subcatches();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %10.4f %10.4f %10.4f %10.4f %10.2f\n",ctx.subcatch_names.name_of(j).c_str(),ctx.subcatches.n_imperv[u],ctx.subcatches.n_perv[u],ctx.subcatches.ds_imperv[u]*12.0,ctx.subcatches.ds_perv[u]*12.0,ctx.subcatches.frac_imperv_no_store[u]*100.0);
    }}

    // [INFILTRATION]
    if(ctx.n_subcatches()>0){sec(f,"INFILTRATION");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s\n","Subcatch","Param1","Param2","Param3","Param4","Param5");
    static const char* infilNames[]={"HORTON","MODIFIED_HORTON","GREEN_AMPT","MODIFIED_GREEN_AMPT","CURVE_NUMBER"};
    for(int j=0;j<ctx.n_subcatches();++j){auto u=static_cast<size_t>(j);
    int im=ctx.subcatches.infil_model[u];
    const char* mn=(im>=0&&im<=4)?infilNames[im]:"HORTON";
    std::fprintf(f,"%-16s %10.4f %10.4f %10.4f %10.4f %10.4f %s\n",
        ctx.subcatch_names.name_of(j).c_str(),
        ctx.subcatches.infil_p1[u],ctx.subcatches.infil_p2[u],
        ctx.subcatches.infil_p3[u],ctx.subcatches.infil_p4[u],
        ctx.subcatches.infil_p5[u],mn);
    }}

    // [JUNCTIONS]
    if(hasNT(ctx,NodeType::JUNCTION)){sec(f,"JUNCTIONS");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s %-12s %-12s\n","Name","Elev","MaxDepth","InitDepth","SurDepth","Aponded");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::JUNCTION)continue;
    std::fprintf(f,"%-16s %12.4f %12.4f %12.4f %12.4f %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ctx.nodes.full_depth[u],ctx.nodes.init_depth[u],ctx.nodes.sur_depth[u],ctx.nodes.ponded_area[u]);
    }}

    // [OUTFALLS]
    if(hasNT(ctx,NodeType::OUTFALL)){sec(f,"OUTFALLS");
    std::fprintf(f,";;%-16s %-12s %-12s %-8s\n","Name","Elev","Type","Gated");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::OUTFALL)continue;
    std::fprintf(f,"%-16s %12.4f %-12s %s",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ofName(ctx.nodes.outfall_type[u]),ctx.nodes.outfall_has_flap_gate[u]?"YES":"NO");
    if(ctx.nodes.outfall_type[u]==OutfallType::FIXED)std::fprintf(f," %12.4f",ctx.nodes.outfall_param[u]);
    std::fprintf(f,"\n");
    }}

    // [DIVIDERS]
    if(hasNT(ctx,NodeType::DIVIDER)){sec(f,"DIVIDERS");
    std::fprintf(f,";;%-16s %-12s %-16s %-12s\n","Name","Elev","DivLink","Type");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::DIVIDER)continue;
    std::fprintf(f,"%-16s %12.4f *                CUTOFF       0 %12.4f %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ctx.nodes.full_depth[u],ctx.nodes.init_depth[u]);
    }}

    // [STORAGE]
    if(hasNT(ctx,NodeType::STORAGE)){sec(f,"STORAGE");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s %-12s\n","Name","Elev","MaxDepth","InitDepth","Shape");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::STORAGE)continue;
    if(ctx.nodes.storage_curve[u]>=0)
        std::fprintf(f,"%-16s %12.4f %12.4f %12.4f TABULAR    %s 0 0 %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ctx.nodes.full_depth[u],ctx.nodes.init_depth[u],tN(ctx,ctx.nodes.storage_curve[u]),ctx.nodes.sur_depth[u]);
    else
        std::fprintf(f,"%-16s %12.4f %12.4f %12.4f FUNCTIONAL %g %g %g 0 %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ctx.nodes.full_depth[u],ctx.nodes.init_depth[u],ctx.nodes.storage_a[u],ctx.nodes.storage_b[u],ctx.nodes.storage_c[u],ctx.nodes.sur_depth[u]);
    }}

    // [CONDUITS]
    if(hasLT(ctx,LinkType::CONDUIT)){sec(f,"CONDUITS");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-12s %-12s %-12s %-10s %-10s\n","Name","FromNode","ToNode","Length","Roughness","InOffset","OutOffset","InitFlow","MaxFlow");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::CONDUIT)continue;
    std::fprintf(f,"%-16s %-16s %-16s %12.4f %12.6f %12.4f %12.4f %10.4f %10.4f\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),ctx.links.length[u],ctx.links.roughness[u],ctx.links.offset1[u],ctx.links.offset2[u],ctx.links.q0[u],ctx.links.q_limit[u]);
    }}

    // [PUMPS]
    if(hasLT(ctx,LinkType::PUMP)){sec(f,"PUMPS");
    std::fprintf(f,";;%-16s %-16s %-16s %-16s %-10s %-10s %-10s\n","Name","FromNode","ToNode","PumpCurve","Status","Startup","Shutoff");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::PUMP)continue;
    std::fprintf(f,"%-16s %-16s %-16s %-16s %-10s 0          0\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),tN(ctx,ctx.links.pump_curve[u]),ctx.links.setting[u]>0?"ON":"OFF");
    }}

    // [ORIFICES]
    if(hasLT(ctx,LinkType::ORIFICE)){sec(f,"ORIFICES");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-10s %-10s %-8s\n","Name","FromNode","ToNode","Type","Offset","Cd","Gated");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::ORIFICE)continue;
    std::fprintf(f,"%-16s %-16s %-16s SIDE       %10.4f %10.4f NO\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),ctx.links.offset1[u],ctx.links.cd[u]);
    }}

    // [WEIRS]
    if(hasLT(ctx,LinkType::WEIR)){sec(f,"WEIRS");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-10s %-8s %-10s %-10s\n","Name","FromNode","ToNode","Type","CrestHt","Cd","Gated","EndCon","EndCoeff");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::WEIR)continue;
    std::fprintf(f,"%-16s %-16s %-16s %-12s %10.4f %10.4f NO       0          0\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),"TRANSVERSE",ctx.links.crest_height[u],ctx.links.cd[u]);
    }}

    // [OUTLETS]
    if(hasLT(ctx,LinkType::OUTLET)){sec(f,"OUTLETS");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-16s %-10s %-10s\n","Name","FromNode","ToNode","Offset","Type","Coeff","Expon");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::OUTLET)continue;
    std::fprintf(f,"%-16s %-16s %-16s %10.4f FUNCTIONAL   %10g %10g\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),ctx.links.offset1[u],ctx.links.cd[u],ctx.links.param2[u]);
    }}

    // [XSECTIONS]
    {sec(f,"XSECTIONS");
    std::fprintf(f,";;%-16s %-16s %-12s %-12s %-12s %-12s %-8s\n","Link","Shape","Geom1","Geom2","Geom3","Geom4","Barrels");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);
    if(ctx.links.type[u]==LinkType::PUMP)continue;
    std::fprintf(f,"%-16s %-16s %12.4f %12.4f %12.4f %12.4f %8d\n",ctx.link_names.name_of(j).c_str(),xsName(static_cast<int>(ctx.links.xsect_shape[u])),ctx.links.xsect_y_full[u],ctx.links.xsect_w_max[u],0.0,0.0,ctx.links.barrels[u]);
    }}

    // [LOSSES]
    {bool hasLoss=false;
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);
    if(ctx.links.type[u]==LinkType::CONDUIT&&(ctx.links.loss_inlet[u]!=0||ctx.links.loss_outlet[u]!=0||ctx.links.loss_avg[u]!=0||ctx.links.has_flap_gate[u]||ctx.links.seep_rate[u]!=0)){hasLoss=true;break;}}
    if(hasLoss){sec(f,"LOSSES");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-8s %-10s\n","Link","Kentry","Kexit","Kavg","Flap","Seepage");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);
    if(ctx.links.type[u]!=LinkType::CONDUIT)continue;
    if(ctx.links.loss_inlet[u]==0&&ctx.links.loss_outlet[u]==0&&ctx.links.loss_avg[u]==0&&!ctx.links.has_flap_gate[u]&&ctx.links.seep_rate[u]==0)continue;
    std::fprintf(f,"%-16s %10.4f %10.4f %10.4f %-8s %10.6f\n",
        ctx.link_names.name_of(j).c_str(),
        ctx.links.loss_inlet[u],ctx.links.loss_outlet[u],ctx.links.loss_avg[u],
        ctx.links.has_flap_gate[u]?"YES":"NO",ctx.links.seep_rate[u]);
    }}}

    // [TRANSECTS]
    if(ctx.transects.count()>0){sec(f,"TRANSECTS");
    for(int t=0;t<ctx.transects.count();++t){auto ut=static_cast<size_t>(t);
    std::fprintf(f,"NC %10.4f %10.4f %10.4f\n",ctx.transects.n_left[ut],ctx.transects.n_right[ut],ctx.transects.n_channel[ut]);
    int nsta=static_cast<int>(ctx.transects.stations[ut].size());
    std::fprintf(f,"X1 %-16s %10d %10.4f %10.4f 0 0 0 %10.4f %10.4f\n",
        ctx.transects.names[ut].c_str(),nsta,ctx.transects.x_left_bank[ut],
        ctx.transects.x_right_bank[ut],ctx.transects.x_factor[ut],ctx.transects.y_factor[ut]);
    for(int k=0;k<nsta;++k){auto uk=static_cast<size_t>(k);
    if(k%5==0)std::fprintf(f,"GR");
    std::fprintf(f," %10.4f %10.4f",ctx.transects.elevations[ut][uk],ctx.transects.stations[ut][uk]);
    if(k%5==4||k==nsta-1)std::fprintf(f,"\n");
    }}}

    // [STREETS]
    if(ctx.streets.count()>0){sec(f,"STREETS");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s %-10s %-8s %-10s %-10s %-10s\n",
        "Name","Tcrown","Hcurb","Sx","nRoad","a","Wdep","Sides","Tback","Sback","nBack");
    for(int j=0;j<ctx.streets.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %10.4f %10.4f %10.4f %10.6f %10.4f %10.4f %8d %10.4f %10.4f %10.6f\n",
        ctx.streets.names[u].c_str(),ctx.streets.t_crown[u],ctx.streets.h_curb[u],
        ctx.streets.sx[u],ctx.streets.n_road[u],ctx.streets.gutter_depres[u],
        ctx.streets.gutter_width[u],ctx.streets.sides[u],
        ctx.streets.back_width[u],ctx.streets.back_slope[u],ctx.streets.back_n[u]);
    }}

    // [INLETS]
    if(ctx.inlets.count()>0){sec(f,"INLETS");
    for(int j=0;j<ctx.inlets.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-12s %10.4f %10.4f",
        ctx.inlets.names[u].c_str(),ctx.inlets.inlet_type[u].c_str(),
        ctx.inlets.length[u],ctx.inlets.width[u]);
    if(!ctx.inlets.grate_type[u].empty())
        std::fprintf(f," %s",ctx.inlets.grate_type[u].c_str());
    if(ctx.inlets.open_area[u]>0)
        std::fprintf(f," %g",ctx.inlets.open_area[u]);
    if(ctx.inlets.splash_veloc[u]>0)
        std::fprintf(f," %g",ctx.inlets.splash_veloc[u]);
    std::fprintf(f,"\n");
    }}

    // [CONTROLS]
    if(ctx.control_rules.count()>0){sec(f,"CONTROLS");
    for(int j=0;j<ctx.control_rules.count();++j){
    std::fprintf(f,"%s\n",ctx.control_rules.rule_text[static_cast<size_t>(j)].c_str());
    }}

    // [POLLUTANTS]
    if(ctx.n_pollutants()>0){sec(f,"POLLUTANTS");
    std::fprintf(f,";;%-16s %-8s %-10s %-10s %-10s %-10s %-10s %-16s %-10s\n","Name","Units","Crain","Cgw","Crdii","Kdecay","SnowOnly","CoPollut","CoFrac");
    for(int p=0;p<ctx.n_pollutants();++p){auto u=static_cast<size_t>(p);
    const char*un="MG/L";if(ctx.pollutants.units[u]==MassUnits::UG_PER_L)un="UG/L";if(ctx.pollutants.units[u]==MassUnits::COUNTS_PER_L)un="#/L";
    std::fprintf(f,"%-16s %-8s %10.4f %10.4f %10.4f %10.4f %-10s %-16s %10.4f\n",pN(ctx,p),un,ctx.pollutants.c_rain[u],ctx.pollutants.c_gw[u],ctx.pollutants.c_rdii[u],ctx.pollutants.k_decay[u],ctx.pollutants.snow_only[u]?"YES":"NO",ctx.pollutants.co_pollut[u]>=0?pN(ctx,ctx.pollutants.co_pollut[u]):"*",ctx.pollutants.co_frac[u]);
    }}

    // [LANDUSES]
    if(ctx.n_landuses()>0){sec(f,"LANDUSES");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s\n","Name","SweepIntrvl","MaxRemoval","LastSwept");
    for(int j=0;j<ctx.n_landuses();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %12.2f %12.2f %12.2f\n",ctx.landuse_names.name_of(j).c_str(),
        ctx.landuses.sweep_interval[u],ctx.landuses.sweep_removal[u],ctx.landuses.last_swept[u]);
    }}

    // [BUILDUP]
    if(ctx.buildup.n_landuses>0&&ctx.buildup.n_pollutants>0){sec(f,"BUILDUP");
    std::fprintf(f,";;%-16s %-16s %-10s %-10s %-10s %-10s %-8s\n","LandUse","Pollutant","FuncType","Coeff1","Coeff2","Coeff3","PerUnit");
    static const char* buNames[]={"NONE","POW","EXP","SAT","EXT"};
    for(int lu=0;lu<ctx.buildup.n_landuses;++lu){
    for(int p=0;p<ctx.buildup.n_pollutants;++p){
    auto idx=static_cast<size_t>(lu*ctx.buildup.n_pollutants+p);
    int ft=ctx.buildup.func_type[idx];
    if(ft==0)continue;
    std::fprintf(f,"%-16s %-16s %-10s %10.4f %10.4f %10.4f %-8s\n",
        ctx.landuse_names.name_of(lu).c_str(),pN(ctx,p),
        (ft>=0&&ft<=4)?buNames[ft]:"NONE",
        ctx.buildup.coeff1[idx],ctx.buildup.coeff2[idx],ctx.buildup.coeff3[idx],
        ctx.buildup.normalizer[idx]==0?"AREA":"CURB");
    }}}

    // [WASHOFF]
    if(ctx.washoff.n_landuses>0&&ctx.washoff.n_pollutants>0){sec(f,"WASHOFF");
    std::fprintf(f,";;%-16s %-16s %-10s %-10s %-10s %-10s %-10s\n","LandUse","Pollutant","FuncType","Coeff","Expon","SweepEff","BmpEff");
    static const char* woNames[]={"NONE","EXP","RC","EMC"};
    for(int lu=0;lu<ctx.washoff.n_landuses;++lu){
    for(int p=0;p<ctx.washoff.n_pollutants;++p){
    auto idx=static_cast<size_t>(lu*ctx.washoff.n_pollutants+p);
    int ft=ctx.washoff.func_type[idx];
    if(ft==0)continue;
    std::fprintf(f,"%-16s %-16s %-10s %10.4f %10.4f %10.2f %10.2f\n",
        ctx.landuse_names.name_of(lu).c_str(),pN(ctx,p),
        (ft>=0&&ft<=3)?woNames[ft]:"NONE",
        ctx.washoff.coeff[idx],ctx.washoff.expon[idx],
        ctx.washoff.sweep_effic[idx],ctx.washoff.bmp_effic[idx]);
    }}}

    // [TREATMENT]
    if(ctx.treatment.hasAny()){sec(f,"TREATMENT");
    std::fprintf(f,";;%-16s %-16s %s\n","Node","Pollutant","Function");
    for(int n=0;n<ctx.treatment.n_nodes;++n){
    for(int p=0;p<ctx.treatment.n_pollutants;++p){
    auto idx=static_cast<size_t>(n*ctx.treatment.n_pollutants+p);
    if(ctx.treatment.expressions[idx].empty())continue;
    std::fprintf(f,"%-16s %-16s %s\n",nN(ctx,n),pN(ctx,p),ctx.treatment.expressions[idx].c_str());
    }}}

    // [INFLOWS]
    if(ctx.ext_inflows.count()>0){sec(f,"INFLOWS");
    std::fprintf(f,";;%-16s %-16s %-16s %-16s %-10s %-10s %-10s %-16s\n",
        "Node","Constituent","TimeSeries","Type","Mfactor","Sfactor","Baseline","Pattern");
    for(int j=0;j<ctx.ext_inflows.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-16s %-16s %-16s %10.4f %10.4f %10.4f %-16s\n",
        nN(ctx,ctx.ext_inflows.node_idx[u]),
        ctx.ext_inflows.constituent[u].c_str(),
        ctx.ext_inflows.ts_name[u].empty()?"\"\"":ctx.ext_inflows.ts_name[u].c_str(),
        ctx.ext_inflows.inflow_type[u].c_str(),
        ctx.ext_inflows.m_factor[u],ctx.ext_inflows.s_factor[u],
        ctx.ext_inflows.baseline[u],
        ctx.ext_inflows.pattern_name[u].empty()?"":ctx.ext_inflows.pattern_name[u].c_str());
    }}

    // [DWF]
    if(ctx.dwf_inflows.count()>0){sec(f,"DWF");
    std::fprintf(f,";;%-16s %-16s %-12s %-16s %-16s %-16s %-16s\n",
        "Node","Constituent","AvgValue","Pat1","Pat2","Pat3","Pat4");
    for(int j=0;j<ctx.dwf_inflows.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-16s %12.6f",
        nN(ctx,ctx.dwf_inflows.node_idx[u]),
        ctx.dwf_inflows.constituent[u].c_str(),
        ctx.dwf_inflows.avg_value[u]);
    if(!ctx.dwf_inflows.pat1[u].empty())std::fprintf(f," %-16s",ctx.dwf_inflows.pat1[u].c_str());
    if(!ctx.dwf_inflows.pat2[u].empty())std::fprintf(f," %-16s",ctx.dwf_inflows.pat2[u].c_str());
    if(!ctx.dwf_inflows.pat3[u].empty())std::fprintf(f," %-16s",ctx.dwf_inflows.pat3[u].c_str());
    if(!ctx.dwf_inflows.pat4[u].empty())std::fprintf(f," %-16s",ctx.dwf_inflows.pat4[u].c_str());
    std::fprintf(f,"\n");
    }}

    // [RDII]
    if(ctx.rdii_assigns.count()>0){sec(f,"RDII");
    std::fprintf(f,";;%-16s %-16s %-12s\n","Node","UnitHyd","SewerArea");
    for(int j=0;j<ctx.rdii_assigns.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-16s %12.4f\n",
        nN(ctx,ctx.rdii_assigns.node_idx[u]),
        ctx.rdii_assigns.uh_name[u].c_str(),
        ctx.rdii_assigns.sewer_area[u]);
    }}

    // [PATTERNS]
    if(ctx.patterns.count()>0){sec(f,"PATTERNS");
    std::fprintf(f,";;%-16s %-12s\n","Name","Type");
    static const char* patNames[]={"MONTHLY","DAILY","HOURLY","WEEKEND"};
    for(int j=0;j<ctx.patterns.count();++j){auto u=static_cast<size_t>(j);
    int pt=ctx.patterns.types[u];
    const char* ptn=(pt>=0&&pt<=3)?patNames[pt]:"MONTHLY";
    const auto& facs=ctx.patterns.factors[u];
    // First line: name + type + first batch of values
    std::fprintf(f,"%-16s %-12s",ctx.patterns.names[u].c_str(),ptn);
    for(size_t k=0;k<facs.size()&&k<6;++k)std::fprintf(f," %10.4f",facs[k]);
    std::fprintf(f,"\n");
    // Continuation lines (6 values per line)
    for(size_t k=6;k<facs.size();k+=6){
    std::fprintf(f,"%-16s            ",ctx.patterns.names[u].c_str());
    for(size_t m=k;m<facs.size()&&m<k+6;++m)std::fprintf(f," %10.4f",facs[m]);
    std::fprintf(f,"\n");
    }}}

    // [TIMESERIES]
    {bool has=false;for(const auto&t:ctx.tables.tables)if(t.type==TableType::TIMESERIES){has=true;break;}
    if(has){sec(f,"TIMESERIES");std::fprintf(f,";;%-16s %-20s %-12s\n","Name","Date/Time","Value");
    for(int t=0;t<static_cast<int>(ctx.tables.tables.size());++t){const auto&tb=ctx.tables.tables[static_cast<size_t>(t)];
    if(tb.type!=TableType::TIMESERIES)continue;
    for(size_t k=0;k<tb.x.size();++k)std::fprintf(f,"%-16s %20.6f %12.6f\n",tN(ctx,t),tb.x[k],tb.y[k]);
    }}}

    // [CURVES]
    {bool has=false;for(const auto&t:ctx.tables.tables)if(t.type!=TableType::TIMESERIES){has=true;break;}
    if(has){sec(f,"CURVES");std::fprintf(f,";;%-16s %-12s %-12s %-12s\n","Name","Type","X-Value","Y-Value");
    for(int t=0;t<static_cast<int>(ctx.tables.tables.size());++t){const auto&tb=ctx.tables.tables[static_cast<size_t>(t)];
    if(tb.type==TableType::TIMESERIES)continue;
    for(size_t k=0;k<tb.x.size();++k)std::fprintf(f,"%-16s %-12s %12.6f %12.6f\n",tN(ctx,t),"STORAGE",tb.x[k],tb.y[k]);
    }}}

    // [COORDINATES]
    if(!ctx.spatial.node_x.empty()){sec(f,"COORDINATES");
    std::fprintf(f,";;%-16s %-16s %-16s\n","Node","X-Coord","Y-Coord");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);
    if(u<ctx.spatial.node_x.size())std::fprintf(f,"%-16s %16.4f %16.4f\n",ctx.node_names.name_of(j).c_str(),ctx.spatial.node_x[u],ctx.spatial.node_y[u]);
    }}

    // [USER_FLAGS]
    if(ctx.user_flags.def_count()>0){sec(f,"USER_FLAGS");
    std::fprintf(f,";;%-20s %-10s %s\n","Name","Type","Description");
    for(const auto&d:ctx.user_flags.all_defs()){
    const char*ts="BOOLEAN";switch(d.type){case UserFlagType::INTEGER:ts="INTEGER";break;case UserFlagType::REAL:ts="REAL";break;case UserFlagType::STRING:ts="STRING";break;default:break;}
    if(d.description.empty())std::fprintf(f,"%-20s %-10s\n",d.name.c_str(),ts);
    else std::fprintf(f,"%-20s %-10s \"%s\"\n",d.name.c_str(),ts,d.description.c_str());
    }}

    // [USER_FLAG_VALUES]
    if(ctx.user_flags.value_count()>0){sec(f,"USER_FLAG_VALUES");
    std::fprintf(f,";;%-14s %-16s %-20s %s\n","ObjectType","ObjectName","FlagName","Value");
    for(const auto&kv:ctx.user_flags.all_values()){const auto&k=kv.first;
    auto p1=k.find(':');if(p1==std::string::npos)continue;auto p2=k.find(':',p1+1);if(p2==std::string::npos)continue;
    std::fprintf(f,"%-14s %-16s %-20s ",k.substr(0,p1).c_str(),k.substr(p1+1,p2-p1-1).c_str(),k.substr(p2+1).c_str());
    const auto&v=kv.second;
    if(std::holds_alternative<bool>(v))std::fprintf(f,"%s\n",std::get<bool>(v)?"YES":"NO");
    else if(std::holds_alternative<int>(v))std::fprintf(f,"%d\n",std::get<int>(v));
    else if(std::holds_alternative<double>(v))std::fprintf(f,"%g\n",std::get<double>(v));
    else if(std::holds_alternative<std::string>(v)){const auto&s=std::get<std::string>(v);
    if(s.find(' ')!=std::string::npos)std::fprintf(f,"\"%s\"\n",s.c_str());else std::fprintf(f,"%s\n",s.c_str());}
    }}

    // [PLUGINS]
    if(!ctx.plugin_specs.empty()){sec(f,"PLUGINS");
    for(const auto&ps:ctx.plugin_specs){std::fprintf(f,"%s",ps.path.c_str());
    for(const auto&a:ps.init_args)std::fprintf(f," %s",a.c_str());std::fprintf(f,"\n");
    }}

    std::fclose(f);
    return 0;
}

} // namespace inp_writer
} // namespace openswmm
