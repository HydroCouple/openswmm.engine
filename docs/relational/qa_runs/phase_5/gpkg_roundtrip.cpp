/**
 * @file gpkg_roundtrip.cpp
 * @brief Phase-5 GPKG round-trip parity harness.
 *
 * Drives the REAL production paths via the public C API:
 *   golden : openswmm <inp> -> run -> golden.out                (INP input)
 *   export : open <inp>, swmm_model_write_with_plugin -> .gpkg  (GPKG output)
 *   rt     : open <.gpkg> with the GPKG input plugin -> run -> rt.out
 * The shell driver then byte-compares golden.out vs rt.out.
 *
 * Usage: gpkg_roundtrip <model.inp> <workdir>
 */
#include "openswmm_engine.h"

#include <cstdio>
#include <cstring>
#include <string>

static const char* GPKG_ID = "org.hydrocouple.openswmm.plugins.geopackage";

// Run a full simulation; `plugin` empty/NULL = built-in .inp reader.
static int run(const char* in, const char* plugin,
               const std::string& rpt, const std::string& out) {
    SWMM_Engine e = swmm_engine_create();
    if (!e) { std::fprintf(stderr, "create failed\n"); return 1; }
    const char* pl = (plugin && plugin[0]) ? plugin : nullptr;
    std::fprintf(stderr, "  [run] open\n"); std::fflush(stderr);
    int rc = swmm_engine_open(e, in, rpt.c_str(), out.c_str(), pl);
    if (rc != SWMM_OK) {
        std::fprintf(stderr, "open(%s) failed: %s\n", in, swmm_get_last_error_msg(e));
        swmm_engine_destroy(e); return rc;
    }
    std::fprintf(stderr, "  [run] initialize\n"); std::fflush(stderr);
    rc = swmm_engine_initialize(e);
    if (rc != SWMM_OK) { std::fprintf(stderr, "init failed: %s\n", swmm_get_last_error_msg(e));
                         swmm_engine_close(e); swmm_engine_destroy(e); return rc; }
    std::fprintf(stderr, "  [run] start\n"); std::fflush(stderr);
    rc = swmm_engine_start(e, 1);
    if (rc != SWMM_OK) { std::fprintf(stderr, "start failed: %s\n", swmm_get_last_error_msg(e));
                         swmm_engine_close(e); swmm_engine_destroy(e); return rc; }
    std::fprintf(stderr, "  [run] step-loop\n"); std::fflush(stderr);
    double el = 0.0;
    while (true) {
        rc = swmm_engine_step(e, &el);
        if (rc != SWMM_OK) { std::fprintf(stderr, "step failed: %s\n", swmm_get_last_error_msg(e)); break; }
        if (el <= 0.0) break;
    }
    swmm_engine_end(e);
    swmm_engine_report(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return rc;
}

// Open the INP, then write the loaded model out to a GeoPackage.
static int export_gpkg(const char* inp, const std::string& gpkg, const std::string& rpt) {
    SWMM_Engine e = swmm_engine_create();
    if (!e) { std::fprintf(stderr, "create failed\n"); return 1; }
    int rc = swmm_engine_open(e, inp, rpt.c_str(), "", nullptr);
    if (rc != SWMM_OK) {
        std::fprintf(stderr, "export-open(%s) failed: %s\n", inp, swmm_get_last_error_msg(e));
        swmm_engine_destroy(e); return rc;
    }
    rc = swmm_model_write_with_plugin(e, gpkg.c_str(), GPKG_ID);
    if (rc != SWMM_OK)
        std::fprintf(stderr, "write_with_plugin failed: %s\n", swmm_get_last_error_msg(e));
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return rc;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <model.inp> <workdir>\n", argv[0]); return 2; }
    std::string inp = argv[1];
    std::string wd  = argv[2];

    std::string golden_out = wd + "/golden.out";
    std::string rt_out     = wd + "/rt.out";
    std::string gpkg       = wd + "/model.gpkg";

    std::fprintf(stderr, "[phase] golden\n"); std::fflush(stderr);
    int rc = run(inp.c_str(), "", wd + "/golden.rpt", golden_out);
    if (rc != SWMM_OK) { std::fprintf(stderr, "GOLDEN FAILED\n"); return rc; }

    std::fprintf(stderr, "[phase] export\n"); std::fflush(stderr);
    rc = export_gpkg(inp.c_str(), gpkg, wd + "/export.rpt");
    if (rc != SWMM_OK) { std::fprintf(stderr, "EXPORT FAILED\n"); return rc; }

    std::fprintf(stderr, "[phase] roundtrip-run\n"); std::fflush(stderr);
    rc = run(gpkg.c_str(), GPKG_ID, wd + "/rt.rpt", rt_out);
    if (rc != SWMM_OK) { std::fprintf(stderr, "ROUNDTRIP RUN FAILED\n"); return rc; }

    std::printf("OK golden=%s rt=%s\n", golden_out.c_str(), rt_out.c_str());
    return 0;
}
