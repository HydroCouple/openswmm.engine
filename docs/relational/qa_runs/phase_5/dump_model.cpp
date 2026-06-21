/** Dump per-node/link input fields after open+initialize, for round-trip diffing. */
#include "openswmm_engine.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <model> [plugin_id]\n", argv[0]); return 2; }
    const char* pl = (argc > 2 && argv[2][0]) ? argv[2] : nullptr;
    SWMM_Engine e = swmm_engine_create();
    if (swmm_engine_open(e, argv[1], "/tmp/dump.rpt", "", pl) != SWMM_OK) {
        std::fprintf(stderr, "open fail: %s\n", swmm_get_last_error_msg(e)); return 1;
    }
    swmm_engine_initialize(e);
    int nn = swmm_node_count(e), nl = swmm_link_count(e);
    std::printf("NODES %d\n", nn);
    for (int i = 0; i < nn; ++i) {
        int t = -1; double inv=0, md=0, id=0, sd=0, pa=0, fv=0;
        swmm_node_get_type(e, i, &t);
        swmm_node_get_invert_elev(e, i, &inv);
        swmm_node_get_max_depth(e, i, &md);
        swmm_node_get_initial_depth(e, i, &id);
        swmm_node_get_surcharge_depth(e, i, &sd);
        swmm_node_get_ponded_area(e, i, &pa);
        swmm_node_get_full_volume(e, i, &fv);
        double op = 0; int ot = -1, og = -1;
        if (t == 1 /*OUTFALL*/) {
            swmm_node_get_outfall_type(e, i, &ot);
            swmm_node_get_outfall_param(e, i, &op);
            swmm_node_get_outfall_flap_gate(e, i, &og);
        }
        std::printf("N%04d t=%d inv=%.17g md=%.17g id=%.17g sd=%.17g pa=%.17g fv=%.17g ot=%d op=%.17g og=%d\n",
                    i, t, inv, md, id, sd, pa, fv, ot, op, og);
    }
    std::printf("LINKS %d\n", nl);
    for (int i = 0; i < nl; ++i) {
        int t = -1, fn = -1, tn = -1; double o1=0, o2=0, ln=0, rg=0;
        swmm_link_get_type(e, i, &t);
        swmm_link_get_from_node(e, i, &fn);
        swmm_link_get_to_node(e, i, &tn);
        swmm_link_get_offset_up(e, i, &o1);
        swmm_link_get_offset_dn(e, i, &o2);
        swmm_link_get_length(e, i, &ln);
        swmm_link_get_roughness(e, i, &rg);
        int shp = -1; double g1=0, g2=0, g3=0, g4=0;
        swmm_link_get_xsect(e, i, &shp, &g1, &g2, &g3, &g4);
        std::printf("L%04d t=%d fn=%d tn=%d o1=%.17g o2=%.17g len=%.17g n=%.17g "
                    "shp=%d g1=%.17g g2=%.17g g3=%.17g g4=%.17g\n",
                    i, t, fn, tn, o1, o2, ln, rg, shp, g1, g2, g3, g4);
    }
    int ns = swmm_subcatch_count(e);
    std::printf("SUBCATCH %d\n", ns);
    for (int i = 0; i < ns; ++i) {
        int outlet = -1; double ar=0, w=0, sl=0, ip=0, ni=0, np2=0, dsi=0, dsp=0;
        double f0=0, fmin=0, dec=0, dry=0;
        swmm_subcatch_get_area(e, i, &ar);
        swmm_subcatch_get_width(e, i, &w);
        swmm_subcatch_get_slope(e, i, &sl);
        swmm_subcatch_get_imperv_pct(e, i, &ip);
        swmm_subcatch_get_n_imperv(e, i, &ni);
        swmm_subcatch_get_n_perv(e, i, &np2);
        swmm_subcatch_get_ds_imperv(e, i, &dsi);
        swmm_subcatch_get_ds_perv(e, i, &dsp);
        swmm_subcatch_get_infil_horton(e, i, &f0, &fmin, &dec, &dry);
        swmm_subcatch_get_outlet(e, i, &outlet);
        std::printf("S%04d out=%d ar=%.17g w=%.17g sl=%.17g ip=%.17g ni=%.17g npv=%.17g "
                    "dsi=%.17g dsp=%.17g f0=%.17g fmin=%.17g dec=%.17g dry=%.17g\n",
                    i, outlet, ar, w, sl, ip, ni, np2, dsi, dsp, f0, fmin, dec, dry);
    }
    int nt = swmm_table_count(e);
    for (int i = 0; i < nt; ++i) {
        int tp = -1, pc = 0;
        swmm_table_get_type(e, i, &tp);
        swmm_table_get_point_count(e, i, &pc);
        const char* id = swmm_table_id(e, i);
        for (int p = 0; p < pc; ++p) {
            double x = 0, y = 0; swmm_table_get_point(e, i, p, &x, &y);
            std::printf("TS %s type=%d p=%d x=%.17g y=%.17g\n", id ? id : "?", tp, p, x, y);
        }
    }
    swmm_engine_close(e); swmm_engine_destroy(e);
    return 0;
}
