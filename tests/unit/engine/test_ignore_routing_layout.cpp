/**
 * @file test_ignore_routing_layout.cpp
 * @brief IGNORE_ROUTING skips the routing-layout checks, as legacy does.
 *
 * Legacy runs flowrout_init and toposort_sortLinks only from routing_open,
 * which swmm_start calls only when routing is not ignored (swmm5.c:743, 760).
 * Its adverse-slope, tree-layout, outlet and routing-loop checks therefore
 * never run with IGNORE_ROUTING YES. v6 ran them anyway, so the benchmarks
 * deck 3525-nodes (KINWAVE, adverse conduits, routing ignored) failed with
 * ERROR 115 where legacy runs it.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "ignore_routing_out"; // *_out/ is the gitignored scratch convention

/// A KINWAVE network whose first conduit runs uphill (J1 100 ft, J2 101 ft).
std::string deck(bool ignore_routing) {
    return std::string(R"([OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         KINWAVE
START_DATE           01/01/2020
START_TIME           00:00:00
END_DATE             01/01/2020
END_TIME             01:00:00
REPORT_STEP          00:15:00
ROUTING_STEP         00:00:30
IGNORE_ROUTING       )") +
           (ignore_routing ? "YES" : "NO") + R"(

[JUNCTIONS]
J1  100  5  0  0  0
J2  101  5  0  0  0

[OUTFALLS]
O1  99  FREE

[CONDUITS]
C1  J1  J2  100  0.013  0  0  0  0
C2  J2  O1  100  0.013  0  0  0  0

[XSECTIONS]
C1  CIRCULAR  1  0  0  0  1
C2  CIRCULAR  1  0  0  0  1
)";
}

/// Open and initialize the deck; returns the initialize status and every
/// error the engine collected.
int openAndInitialize(const std::string& base, bool ignore_routing, std::string& errors) {
    fs::create_directories(kOutDir);
    const std::string inp = (fs::path(kOutDir) / (base + ".inp")).string();
    std::ofstream(inp) << deck(ignore_routing);
    SWMM_Engine e = swmm_engine_create();
    int rc =
        swmm_engine_open(e, inp.c_str(), (fs::path(kOutDir) / (base + ".rpt")).string().c_str(),
                         (fs::path(kOutDir) / (base + ".out")).string().c_str(), nullptr);
    if (rc == 0)
        rc = swmm_engine_initialize(e);
    for (int i = 0; i < swmm_get_error_count(e); ++i)
        if (const char* m = swmm_get_error_at(e, i))
            errors += std::string(m) + "\n";
    if (const char* last = swmm_get_last_error_msg(e))
        errors += last;
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return rc;
}

} // namespace

TEST(IgnoreRoutingLayout, AdverseKinwaveConduitIsAnErrorWhenRouting) {
    std::string errors;
    EXPECT_NE(openAndInitialize("routed", false, errors), 0);
    EXPECT_NE(errors.find("ERROR 115"), std::string::npos) << errors;
}

TEST(IgnoreRoutingLayout, AdverseKinwaveConduitIsNotCheckedWhenRoutingIsIgnored) {
    std::string errors;
    EXPECT_EQ(openAndInitialize("ignored", true, errors), 0) << errors;
    EXPECT_EQ(errors.find("ERROR 115"), std::string::npos) << errors;
}
