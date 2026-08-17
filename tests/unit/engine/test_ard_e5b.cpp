// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file test_ard_e5b.cpp
 * @brief E5b + IO3 gates: treatment interop at ARD node stores, kdecay
 *        ledger closure, TARGET_DX, the per-cell CSV sidecar, and the
 *        save-as config carry-alongside
 *        (plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md §6 E5b;
 *        TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md IO3).
 *
 * @details Observation paths:
 *          - TreatmentAppliesAtArdNodeStores: [TREATMENT] R = 0.5 at J2 on
 *            a flowing chain. Upstream links (C1/C2) must match the
 *            untreated run BITWISE — dispersion-gate style containment
 *            razor; a blanket store absorb (instead of treated-only)
 *            breaks the round trip and fails here first. Downstream (C5)
 *            must separate, and qual_routing_reacted must book > 0.
 *          - KdecayLedgerCloses: level-pool decay deck; after the run,
 *            init − final ≈ reacted (+outflow ≈ 0) within 2% — the
 *            continuity closure the ledger phase exists for. Falsified by
 *            removing the reacted booking.
 *          - TargetDxChangesTheTransportMesh: TARGET_DX 250 vs default on
 *            a DYNWAVE deck must change the published front (coarser mesh
 *            ⇒ more numerical diffusion); under FLOW_ROUTING FV the key
 *            must warn "ignored" instead.
 *          - DetailedSidecarWrites: DETAILED_OUTPUT produces a CSV with
 *            the header, link-cell rows (kind L) and node-store rows
 *            (kind N) for every species name.
 *          - ConfigCarryAlongsideOnSaveAs (IO3): writeInpFile into a fresh
 *            subdirectory must copy the relative model.ard/model.rxn
 *            alongside, byte-identical to the originals.
 *          - DetailOnlyConfigWarnsUnderLegacy: any_engine_content now
 *            includes DETAILED_OUTPUT/TARGET_DX (bypass surface, lesson
 *            10).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

#include "core/InpWriter.hpp"
#include "core/SWMMEngine.hpp"

namespace {

namespace fsys = std::filesystem;

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kC0     = 10.0;
constexpr double kKdecay = 0.03;

/// The established chain deck (wet junctions — lesson 9). `stagnant` gives
/// the level pool; `treatment_j2` adds an R = 0.5 TSS removal at J2;
/// `routing` selects FLOW_ROUTING.
void write_deck(const char* path, const std::string& pc_lines,
                const std::string& extra_options = "", double kdecay = 0.0,
                bool stagnant = false, bool treatment_j2 = false,
                const char* routing = "DYNWAVE",
                bool treated_inflow = false) {
    std::ofstream f(path);
    f << "[TITLE]\nE5b gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING " << routing << "\n"
      << "QUALITY_SOLVER EULERIAN_ARD\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME "
      << (stagnant ? "00:02:00" : "01:00:00") << "\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:01:00\n"
      << extra_options << "\n";
    if (stagnant) {
        f << "[JUNCTIONS]\n"
          << "J0 10.0 10 1.5 0 0\nJ1 10.0 10 1.5 0 0\n"
          << "J2 10.0 10 1.5 0 0\nJ3 10.0 10 1.5 0 0\n"
          << "J4 10.0 10 1.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 10.0 FIXED 11.5 NO\n\n";
    } else {
        f << "[JUNCTIONS]\n"
          << "J0 10.0 10 1.5 0 0\nJ1 9.4  10 1.5 0 0\nJ2 8.8  10 1.5 0 0\n"
          << "J3 8.2  10 1.5 0 0\nJ4 7.6  10 1.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n";
    }
    f << "[CONDUITS]\n"
      << "C1 J0 J1 500 0.013 0 0 0\nC2 J1 J2 500 0.013 0 0 0\n"
      << "C3 J2 J3 500 0.013 0 0 0\nC4 J3 J4 500 0.013 0 0 0\n"
      << "C5 J4 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n"
      << "C1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 2.0 0 0 0\n"
      << "C3 CIRCULAR 2.0 0 0 0\nC4 CIRCULAR 2.0 0 0 0\n"
      << "C5 CIRCULAR 2.0 0 0 0\n\n";
    // A removal expression (`R = ...`) acts on the node's INFLOW
    // concentration: applyTreatment sets cin = qual_mass_in / qual_vol_in
    // and computes cOut = (1-R)*cin, falling back to the node's own
    // concentration when cin is 0. A treated node with no EXTERNAL inflow
    // therefore has nothing to remove — verified against LEGACY on this
    // deck, where adding the J2 treatment left External Outflow unchanged
    // at 12.119 lb. So the treated node gets its own lateral inflow with a
    // pollutant concentration. Both the base and treated decks carry it, so
    // the comparison isolates treatment alone.
    if (!stagnant) {
        f << "[INFLOWS]\nJ0 FLOW \"\" FLOW 1.0 1.0 5\n";
        if (treatment_j2 || treated_inflow)
            f << "J2 FLOW \"\" FLOW 1.0 1.0 2\n"
              << "J2 TSS  \"\" CONCEN 1.0 1.0 20\n";
        f << "\n";
    }
    f << "[POLLUTANTS]\n"
      << ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac "
         "Cdwf Cinit\n"
      << "TSS    MG/L  0     0   0     " << kdecay
      << "      NO       *        0      0    " << kC0 << "\n\n";
    if (treatment_j2) f << "[TREATMENT]\nJ2 TSS R = 0.5\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

void write_file(const char* path, const std::string& body) {
    std::ofstream c(path);
    c << body;
}

struct RunRecord {
    std::vector<std::vector<double>> tss_link;  ///< [link][step]
    std::vector<std::string> warnings;
    double reacted = 0.0, init = 0.0, final_ = 0.0, outflow = 0.0;
    bool ok = false;
};

RunRecord run_recording(const char* inp, const char* rpt, const char* out) {
    RunRecord rec;
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) { ADD_FAILURE() << "engine create"; return rec; }
    bool ok = swmm_engine_open(e, inp, rpt, out, nullptr) == SWMM_OK;
    if (!ok) ADD_FAILURE() << "open failed for " << inp;
    if (ok && (swmm_engine_initialize(e) != SWMM_OK ||
               swmm_engine_start(e, 1) != SWMM_OK)) {
        ADD_FAILURE() << "init/start failed for " << inp;
        ok = false;
    }
    if (ok) {
        auto& ctx = as_cpp_engine(e).context();
        const int nl = ctx.n_links();
        const int np = ctx.n_pollutants();
        rec.tss_link.assign(static_cast<std::size_t>(nl), {});
        double elapsed = 0.0;
        int guard = 0;
        do {
            if (swmm_engine_step(e, &elapsed) != SWMM_OK) {
                ADD_FAILURE() << "step failed for " << inp;
                ok = false;
                break;
            }
            for (int l = 0; l < nl; ++l)
                if (np > 0)
                    rec.tss_link[static_cast<std::size_t>(l)].push_back(
                        ctx.links.conc[static_cast<std::size_t>(l * np)]);
        } while (elapsed > 0.0 && ++guard < 20000);
        if (ok) swmm_engine_end(e);
        const auto& mb = ctx.mass_balance;
        if (!mb.qual_routing_reacted.empty()) rec.reacted = mb.qual_routing_reacted[0];
        if (!mb.qual_routing_init.empty())    rec.init    = mb.qual_routing_init[0];
        if (!mb.qual_routing_final.empty())   rec.final_  = mb.qual_routing_final[0];
        if (!mb.qual_routing_outflow.empty()) rec.outflow = mb.qual_routing_outflow[0];
        rec.warnings = ctx.warnings;
    }
    swmm_engine_destroy(e);
    rec.ok = ok;
    return rec;
}

bool has_needle(const std::vector<std::string>& v, const std::string& n) {
    for (const auto& s : v)
        if (s.find(n) != std::string::npos) return true;
    return false;
}

double integratedAbsDiff(const std::vector<double>& a,
                         const std::vector<double>& b) {
    double s = 0.0;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) s += std::fabs(a[i] - b[i]);
    return s;
}
double integrated(const std::vector<double>& a) {
    double s = 0.0;
    for (const double x : a) s += x;
    return s;
}

constexpr int kC1 = 0, kC2 = 1, kC5 = 4;

// ---------------------------------------------------------------------------
// Gate 1 — treatment at an ARD node store: contained, effective, booked.
// ---------------------------------------------------------------------------
TEST(ArdE5bTest, TreatmentAppliesAtArdNodeStores) {
    write_deck("_e5b_tr_base.inp", "", "", 0.0, false, /*treatment_j2=*/false,
               "DYNWAVE", /*treated_inflow=*/true);
    write_deck("_e5b_tr.inp", "", "", 0.0, false, /*treatment_j2=*/true);
    const auto base = run_recording("_e5b_tr_base.inp", "_e5b_tr_base.rpt",
                                    "_e5b_tr_base.out");
    const auto tr   = run_recording("_e5b_tr.inp", "_e5b_tr.rpt",
                                    "_e5b_tr.out");
    ASSERT_TRUE(base.ok);
    ASSERT_TRUE(tr.ok);

    // Containment razor: treatment lives at J2; C1/C2 sit upstream in their
    // own chains with no reverse flow. A blanket (all-nodes) absorb breaks
    // the conc→mass round trip everywhere and fails HERE first.
    ASSERT_EQ(base.tss_link[kC1].size(), tr.tss_link[kC1].size());
    for (std::size_t t = 0; t < base.tss_link[kC1].size(); ++t) {
        ASSERT_EQ(base.tss_link[kC1][t], tr.tss_link[kC1][t])
            << "C1 diverged at step " << t;
        ASSERT_EQ(base.tss_link[kC2][t], tr.tss_link[kC2][t])
            << "C2 diverged at step " << t;
    }

    // Effect: downstream of the treated node the front must separate hard.
    const double sep  = integratedAbsDiff(base.tss_link[kC5], tr.tss_link[kC5]);
    const double norm = integrated(base.tss_link[kC5]);
    ASSERT_GT(norm, 0.0);
    EXPECT_GT(sep, 0.05 * norm)
        << "an R = 0.5 removal at J2 left the downstream trajectory "
           "unchanged — treatment is not reaching the ARD stores "
           "(absorbTreatedNodeConc dead?)";

    // Booking: treatment mass loss must land in the reacted ledger row.
    EXPECT_GT(tr.reacted, 0.0)
        << "treatment removed mass but booked nothing to "
           "qual_routing_reacted";
}

// ---------------------------------------------------------------------------
// Gate 2 — kdecay ledger closure on the level pool.
// ---------------------------------------------------------------------------
TEST(ArdE5bTest, KdecayLedgerCloses) {
    write_deck("_e5b_kd.inp", "", "", kKdecay, /*stagnant=*/true);
    const auto rec = run_recording("_e5b_kd.inp", "_e5b_kd.rpt",
                                   "_e5b_kd.out");
    ASSERT_TRUE(rec.ok);
    ASSERT_GT(rec.init, 0.0) << "no initial stored mass — deck defect";
    // Level pool: no inflow, (near) no outflow — the ledger must close as
    // init ≈ final + reacted. 2% band covers boundary micro-flows.
    const double closure = rec.init - rec.final_ - rec.reacted - rec.outflow;
    EXPECT_LT(std::fabs(closure), 0.02 * rec.init)
        << "quality continuity does not close: init=" << rec.init
        << " final=" << rec.final_ << " reacted=" << rec.reacted
        << " outflow=" << rec.outflow
        << " — the ARD kdecay stage is not booking its removals.";
    EXPECT_GT(rec.reacted, 0.5 * rec.init)
        << "k·t = 3.6 should react away >96% of the initial mass; the "
           "reacted row is implausibly small.";
}

// ---------------------------------------------------------------------------
// Gate 3 — TARGET_DX changes the transport mesh; warns under FV routing.
// ---------------------------------------------------------------------------
TEST(ArdE5bTest, TargetDxChangesTheTransportMesh) {
    write_deck("_e5b_dx_base.inp", "");
    // 50 ft, not 250. FV_MIN_CELLS is a FLOOR of 4 cells per conduit, so on
    // these 500 ft conduits ceil(500/250) = 2 clamps back to 4 — exactly the
    // 4 the default (cell_length 0 => 1, clamped to 4) already produces, and
    // the run came out BIT-IDENTICAL. 500/50 = 10 clears the floor.
    write_file("_e5b_dx.ard", "[TRANSPORT_OPTIONS]\nTARGET_DX 50\n");
    write_deck("_e5b_dx.inp",
               "org.hydrocouple.openswmm.transport.ard "
               "config=\"_e5b_dx.ard\"");
    const auto base = run_recording("_e5b_dx_base.inp", "_e5b_dx_base.rpt",
                                    "_e5b_dx_base.out");
    const auto dx   = run_recording("_e5b_dx.inp", "_e5b_dx.rpt",
                                    "_e5b_dx.out");
    ASSERT_TRUE(base.ok);
    ASSERT_TRUE(dx.ok);
    const double sep  = integratedAbsDiff(base.tss_link[kC5],
                                          dx.tss_link[kC5]);
    const double norm = integrated(base.tss_link[kC5]);
    ASSERT_GT(norm, 0.0);
    EXPECT_GT(sep, 0.002 * norm)
        << "TARGET_DX 50 left the published front unchanged — the key "
           "does not reach the mesh builder.";

    // Under FLOW_ROUTING FV the solver mesh governs: the key must WARN.
    write_file("_e5b_dxf.ard", "[TRANSPORT_OPTIONS]\nTARGET_DX 250\n");
    write_deck("_e5b_dxf.inp",
               "org.hydrocouple.openswmm.transport.ard "
               "config=\"_e5b_dxf.ard\"",
               "", 0.0, false, false, "FV");
    const auto fvr = run_recording("_e5b_dxf.inp", "_e5b_dxf.rpt",
                                   "_e5b_dxf.out");
    ASSERT_TRUE(fvr.ok);
    EXPECT_TRUE(has_needle(fvr.warnings, "TARGET_DX is ignored under"))
        << "TARGET_DX under FLOW_ROUTING FV was silently ignored";
}

// ---------------------------------------------------------------------------
// Gate 4 — the per-cell CSV sidecar writes structured rows.
// ---------------------------------------------------------------------------
TEST(ArdE5bTest, DetailedSidecarWrites) {
    std::error_code ec;
    fsys::remove("_e5b_detail.csv", ec);
    write_file("_e5b_dt.ard",
               "[TRANSPORT_OPTIONS]\nDETAILED_OUTPUT _e5b_detail.csv\n");
    write_deck("_e5b_dt.inp",
               "org.hydrocouple.openswmm.transport.ard "
               "config=\"_e5b_dt.ard\"");
    const auto rec = run_recording("_e5b_dt.inp", "_e5b_dt.rpt",
                                   "_e5b_dt.out");
    ASSERT_TRUE(rec.ok);

    std::ifstream in("_e5b_detail.csv");
    ASSERT_TRUE(in.is_open()) << "sidecar file was never created";
    std::string line;
    ASSERT_TRUE(std::getline(in, line));
    EXPECT_EQ(line, "time_s,element,kind,cell,species,conc");
    int link_rows = 0, node_rows = 0;
    while (std::getline(in, line)) {
        if (line.find(",L,") != std::string::npos &&
            line.find("TSS") != std::string::npos)
            ++link_rows;
        if (line.find(",N,") != std::string::npos &&
            line.find("TSS") != std::string::npos)
            ++node_rows;
    }
    EXPECT_GT(link_rows, 0) << "no link-cell rows in the sidecar";
    EXPECT_GT(node_rows, 0) << "no node-store rows in the sidecar";
}

// ---------------------------------------------------------------------------
// Gate 5 — IO3: save-as copies relative config files alongside.
// ---------------------------------------------------------------------------
TEST(ArdE5bTest, ConfigCarryAlongsideOnSaveAs) {
    const std::string ard_body = "[TRANSPORT_OPTIONS]\nDISPERSION 5\n";
    write_file("_e5b_sv.ard", ard_body);
    write_deck("_e5b_sv.inp",
               "org.hydrocouple.openswmm.transport.ard "
               "config=\"_e5b_sv.ard\"");

    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_e5b_sv.inp", "_e5b_sv.rpt",
                               "_e5b_sv.out", nullptr),
              SWMM_OK);

    std::error_code ec;
    fsys::remove_all("_e5b_saveas", ec);
    fsys::create_directories("_e5b_saveas", ec);
    std::vector<std::string> warnings;
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(
                  as_cpp_engine(e).context(), "_e5b_saveas/model.inp",
                  &warnings),
              0);
    swmm_engine_destroy(e);

    // The relative config reference must not dangle in the destination:
    // the file the model was READ from is copied alongside, byte-equal.
    std::ifstream copied("_e5b_saveas/_e5b_sv.ard");
    ASSERT_TRUE(copied.is_open())
        << "save-as did not carry the component config alongside — the "
           "written config=\"_e5b_sv.ard\" reference dangles.";
    std::stringstream got;
    got << copied.rdbuf();
    EXPECT_EQ(got.str(), ard_body);
}

// ---------------------------------------------------------------------------
// Gate 6 — DETAILED_OUTPUT-only config under LEGACY warns (bypass surface).
// ---------------------------------------------------------------------------
TEST(ArdE5bTest, DetailOnlyConfigWarnsUnderLegacy) {
    write_file("_e5b_lg.ard",
               "[TRANSPORT_OPTIONS]\nDETAILED_OUTPUT _e5b_lg.csv\n");
    write_deck("_e5b_lg.inp",
               "org.hydrocouple.openswmm.transport.ard "
               "config=\"_e5b_lg.ard\"",
               "QUALITY_SOLVER LEGACY\n");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_e5b_lg.inp", "_e5b_lg.rpt",
                               "_e5b_lg.out", nullptr),
              SWMM_OK);
    EXPECT_TRUE(has_needle(as_cpp_engine(e).context().warnings,
                           "QUALITY_SOLVER is not EULERIAN_ARD"))
        << "a DETAILED_OUTPUT-only model.ard under LEGACY ran without a "
           "word (any_engine_content hole)";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 7 — IO3 must not destroy an unrelated file silently. Overwriting is
// REQUIRED (a re-save into a folder holding last save's copy must refresh
// it, or the deck ships stale), so the defense is not "refuse" but "say so".
// Measured before this: a save-as replaced an unrelated model config and
// reported warnings=0. Gate 5 only ever writes into a freshly created empty
// directory, so it cannot see this.
// ---------------------------------------------------------------------------
TEST(ArdE5bTest, SaveAsAnnouncesReplacingADifferentConfig) {
    const std::string ard_body = "[TRANSPORT_OPTIONS]\nDISPERSION 5\n";
    write_file("_e5b_ov.ard", ard_body);
    write_deck("_e5b_ov.inp",
               "org.hydrocouple.openswmm.transport.ard "
               "config=\"_e5b_ov.ard\"");

    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_e5b_ov.inp", "_e5b_ov.rpt",
                               "_e5b_ov.out", nullptr),
              SWMM_OK);

    std::error_code ec;
    fsys::remove_all("_e5b_ovdst", ec);
    fsys::create_directories("_e5b_ovdst", ec);
    // A DIFFERENT file already sits where the copy will land.
    write_file("_e5b_ovdst/_e5b_ov.ard",
               "[TRANSPORT_OPTIONS]\nDISPERSION 999\n");

    std::vector<std::string> w1;
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(
                  as_cpp_engine(e).context(), "_e5b_ovdst/model.inp", &w1), 0);
    bool announced = false;
    for (const auto& w : w1)
        if (w.find("replaced an existing, different") != std::string::npos)
            announced = true;
    EXPECT_TRUE(announced)
        << "save-as overwrote a different pre-existing component config and "
           "said nothing";

    // Idempotence: the same save again replaces an IDENTICAL file, which is
    // the ordinary re-save and must stay quiet.
    std::vector<std::string> w2;
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(
                  as_cpp_engine(e).context(), "_e5b_ovdst/model.inp", &w2), 0);
    for (const auto& w : w2)
        EXPECT_EQ(w.find("replaced an existing, different"), std::string::npos)
            << "an idempotent re-save warned about replacing itself";
    swmm_engine_destroy(e);
}

}  // namespace
