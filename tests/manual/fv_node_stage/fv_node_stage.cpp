// Diagnose FV node-stage steps in the published HGL.
//
// WHY THIS EXISTS
// ---------------
// FV profile plots show the node stage sitting above the adjacent conduit
// water surface. The published node head comes from Routing.cpp:1254-1289:
//
//     head = fv_state_.node_head[n];                  // the solver's own head
//     if (node_passthrough()[n]) {                    // ...overridden ONLY here
//         head = max over WET incident faces of
//                min(cell_zb, face_zb) + cell_h;      // face-consistent stage
//     }
//     depth = head - node_invert;
//
// So the face-consistent reconstruction (commit 3ca9f4ed) covers exactly one
// node class. Every other node publishes the raw solver head, which for a
// pass-through is a MEAN of cell-centre etas and sits ~S0*dx/2 above the face
// stage on steep coarse chains.
//
// This probe answers: WHICH class does each node in a real model land in, and
// how big is the step per class? Node classification is derivable from
// topology alone, so no solver internals are needed -- the pass-through gate
// (ExplicitFvSolver.cpp:143-163, :1818-1834) is:
//
//     junction, no storage volume, EXACTLY 2 incident conduit faces,
//     neither face carrying a culvert inlet or flap gate,
//     no structure flow (no pump/orifice/weir/outlet attached),
//     lateral inflow zero or divertible.
//
// We can see all of that from the public C API. The lateral-inflow and carry
// terms are runtime state, so eligibility computed here is an UPPER BOUND on
// how many nodes actually get the fix -- which is the conservative direction
// for the question being asked.
//
// The step itself is measured against the PUBLISHED link depths, deliberately:
// that is what the HGL plot draws, so it is the quantity the user sees.
//
// Build + run: see RESULTS.md in this directory.
//   ./fv_node_stage <model.inp> <model.out>
// where <model.out> is a completed FV run of that same .inp.

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_output.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum NodeClass {
    CLS_PASSTHRU = 0,   // degree-2 clean junction -> gets the publish-side fix
    CLS_ALG_DRYLAT,     // storage-less junction, NO lateral inflow, no fix
    CLS_ALG_WETLAT,     // storage-less junction WITH lateral inflow, no fix
    CLS_BUCKET_ZEROFACE,// junction with no conduit faces (pump-only wet well)
    CLS_STORAGE,        // storage unit
    CLS_OUTFALL,
    CLS_DIVIDER,
    CLS_COUNT
};

const char* kClassName[CLS_COUNT] = {
    "passthru (fix applies)", "algebraic, no lat inflow", "algebraic, HAS lat inflow",
    "junction, 0 conduits",
    "storage", "outfall", "divider",
};

struct Percentiles { double p50, p90, p99, max; };

Percentiles pct(std::vector<double>& v)
{
    Percentiles p{0, 0, 0, 0};
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    auto at = [&](double f) {
        const auto i = static_cast<std::size_t>(f * double(v.size() - 1) + 0.5);
        return v[std::min(i, v.size() - 1)];
    };
    p.p50 = at(0.50); p.p90 = at(0.90); p.p99 = at(0.99); p.max = v.back();
    return p;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <model.inp> <model.out>\n"
                     "  <model.out> must be a completed FV run of <model.inp>\n",
                     argv[0]);
        return 2;
    }
    const std::string inp = argv[1];
    const std::string out = argv[2];

    // ---- topology, from a throwaway open so the real .out is untouched -----
    SWMM_Engine eng = swmm_engine_create();
    if (!eng) { std::fprintf(stderr, "engine_create failed\n"); return 1; }
    const std::string junkRpt = out + ".probe.rpt";
    const std::string junkOut = out + ".probe.out";
    int err = swmm_engine_open(eng, inp.c_str(), junkRpt.c_str(),
                               junkOut.c_str(), nullptr);
    if (err != SWMM_OK) {
        std::fprintf(stderr, "engine_open failed: %d\n", err);
        swmm_engine_destroy(eng);
        return 1;
    }

    const int nN = swmm_node_count(eng);
    const int nL = swmm_link_count(eng);

    std::vector<int>    conduitDeg(nN, 0);   // incident CONDUIT faces
    std::vector<int>    structDeg(nN, 0);    // incident pump/orifice/weir/outlet
    std::vector<int>    dirty(nN, 0);        // a culvert or flap gate on a face
    std::vector<double> invert(nN, 0.0);
    std::vector<int>    ntype(nN, 0);
    // Adjacent (link, end) pairs, for the stage comparison below.
    std::vector<std::vector<std::pair<int, double>>> adj(nN);  // (link, invert@end)

    for (int n = 0; n < nN; ++n) {
        swmm_node_get_invert_elev(eng, n, &invert[n]);
        swmm_node_get_type(eng, n, &ntype[n]);
    }
    for (int l = 0; l < nL; ++l) {
        int lt = 0, n1 = -1, n2 = -1, culv = 0, flap = 0;
        swmm_link_get_type(eng, l, &lt);
        swmm_link_get_from_node(eng, l, &n1);
        swmm_link_get_to_node(eng, l, &n2);
        if (n1 < 0 || n2 < 0) continue;

        if (lt == SWMM_LINK_CONDUIT) {
            double offUp = 0.0, offDn = 0.0;
            swmm_link_get_offset_up(eng, l, &offUp);
            swmm_link_get_offset_dn(eng, l, &offDn);
            swmm_link_get_culvert_code(eng, l, &culv);
            swmm_link_get_flap_gate(eng, l, &flap);
            ++conduitDeg[n1]; ++conduitDeg[n2];
            if (culv != 0 || flap != 0) { dirty[n1] = 1; dirty[n2] = 1; }
            adj[n1].emplace_back(l, invert[n1] + offUp);
            adj[n2].emplace_back(l, invert[n2] + offDn);
        } else {
            ++structDeg[n1]; ++structDeg[n2];
        }
    }

    // Lateral inflow decides whether a positive step can be PHYSICAL: a
    // storage-less node fed laterally must stand above its outlets to push the
    // water out. Without lateral inflow it has no such excuse.
    std::vector<int> hasLat(nN, 0);
    std::vector<int> cls(nN, CLS_ALG_DRYLAT);
    for (int n = 0; n < nN; ++n) {
        switch (ntype[n]) {
        case SWMM_NODE_STORAGE: cls[n] = CLS_STORAGE; continue;
        case SWMM_NODE_OUTFALL: cls[n] = CLS_OUTFALL; continue;
        case SWMM_NODE_DIVIDER: cls[n] = CLS_DIVIDER; continue;
        default: break;
        }
        if (conduitDeg[n] == 0)                cls[n] = CLS_BUCKET_ZEROFACE;
        else if (conduitDeg[n] == 2 && !dirty[n] && structDeg[n] == 0)
                                               cls[n] = CLS_PASSTHRU;
        else                                   cls[n] = CLS_ALG_DRYLAT;
    }
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    std::remove(junkRpt.c_str());
    std::remove(junkOut.c_str());

    // ---- published stages -------------------------------------------------
    SWMM_Output oh = swmm_output_open(out.c_str());
    if (!oh) { std::fprintf(stderr, "cannot open %s\n", out.c_str()); return 1; }
    const int nPer = swmm_output_get_period_count(oh);
    const int nNo  = swmm_output_get_node_count(oh);
    const int nLo  = swmm_output_get_link_count(oh);
    if (nNo != nN || nLo != nL) {
        std::fprintf(stderr,
                     "MISMATCH: .out has %d nodes / %d links, model has %d / %d "
                     "-- is this .out from this .inp?\n", nNo, nLo, nN, nL);
        swmm_output_close(oh);
        return 1;
    }

    // First pass: which nodes ever see lateral inflow.
    {
        std::vector<float> lat(static_cast<std::size_t>(nN));
        for (int t = 0; t < nPer; ++t) {
            if (swmm_output_get_node_result(oh, t, SWMM_OUT_NODE_LATERAL_INFLOW,
                                            lat.data()) != 0) continue;
            for (int n = 0; n < nN; ++n)
                if (std::fabs(double(lat[static_cast<std::size_t>(n)])) > 1.0e-6)
                    hasLat[n] = 1;
        }
        for (int n = 0; n < nN; ++n)
            if (cls[n] == CLS_ALG_DRYLAT && hasLat[n]) cls[n] = CLS_ALG_WETLAT;
    }

    std::vector<std::vector<double>> stepByCls(CLS_COUNT);
    // The output reader stores results as float; keep the buffers float and
    // widen only for the arithmetic, so no silent narrowing happens here.
    std::vector<float> nodeHead(static_cast<std::size_t>(nN));
    std::vector<float> linkDepth(static_cast<std::size_t>(nL));
    long wetSamples = 0;

    for (int t = 0; t < nPer; ++t) {
        if (swmm_output_get_node_result(oh, t, SWMM_OUT_NODE_HEAD,
                                        nodeHead.data()) != 0) continue;
        if (swmm_output_get_link_result(oh, t, SWMM_OUT_LINK_DEPTH,
                                        linkDepth.data()) != 0) continue;
        for (int n = 0; n < nN; ++n) {
            if (adj[n].empty()) continue;
            // Highest adjacent conduit water surface at this node's end.
            double best = -1.0e30;
            bool wet = false;
            for (const auto& [l, invAtEnd] : adj[n]) {
                const double d = double(linkDepth[static_cast<std::size_t>(l)]);
                if (!(d > 1.0e-4)) continue;      // dry conduit: router FUDGE
                best = std::max(best, invAtEnd + d);
                wet = true;
            }
            if (!wet) continue;
            ++wetSamples;
            // POSITIVE = node stage sits ABOVE the adjacent conduit surface,
            // which is the step the user sees in the profile.
            stepByCls[static_cast<std::size_t>(cls[n])].push_back(
                double(nodeHead[static_cast<std::size_t>(n)]) - best);
        }
    }
    swmm_output_close(oh);

    // ---- report -----------------------------------------------------------
    std::printf("model    : %s\n", inp.c_str());
    std::printf("periods  : %d   nodes: %d   links: %d   wet samples: %ld\n\n",
                nPer, nN, nL, wetSamples);

    std::printf("%-24s %8s %8s   node-stage minus highest adjacent conduit surface\n",
                "node class", "count", "% nodes");
    std::printf("%-24s %8s %8s   %9s %9s %9s %9s\n",
                "", "", "", "p50", "p90", "p99", "max");
    std::printf("%-24s %8s %8s   %9s %9s %9s %9s\n",
                "------------------------", "--------", "--------",
                "---------", "---------", "---------", "---------");

    int total = 0;
    for (int c = 0; c < CLS_COUNT; ++c)
        total += static_cast<int>(std::count(cls.begin(), cls.end(), c));

    for (int c = 0; c < CLS_COUNT; ++c) {
        const int cnt = static_cast<int>(std::count(cls.begin(), cls.end(), c));
        if (cnt == 0) continue;
        Percentiles p = pct(stepByCls[static_cast<std::size_t>(c)]);
        std::printf("%-24s %8d %7.1f%%   %9.4f %9.4f %9.4f %9.4f\n",
                    kClassName[c], cnt,
                    100.0 * double(cnt) / double(total ? total : 1),
                    p.p50, p.p90, p.p99, p.max);
    }

    const int nPass = static_cast<int>(std::count(cls.begin(), cls.end(),
                                                  int(CLS_PASSTHRU)));
    std::printf("\nCOVERAGE: %d of %d nodes (%.1f%%) are pass-through and receive\n"
                "the face-consistent published stage. The remaining %d publish the\n"
                "solver's raw head (Routing.cpp:1254).\n",
                nPass, nN, 100.0 * double(nPass) / double(nN ? nN : 1),
                nN - nPass);
    std::printf("\nNOTE: eligibility here is an UPPER BOUND -- lateral inflow and\n"
                "carry can revoke pass-through at runtime (ExplicitFvSolver.cpp:1818).\n");
    return 0;
}
