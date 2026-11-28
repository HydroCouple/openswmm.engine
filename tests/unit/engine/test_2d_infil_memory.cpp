/**
 * @file      test_2d_infil_memory.cpp
 * @brief     [2D_OPTIONS] INFIL_MEMORY (Track 3C, 2026-11-27): the kernel history advances by what
 *            the surface applied, not by the held offer, when a cell dries during the hold.
 *
 * @details A four-cell Horton mesh takes a one-minute 72 mm/h pulse straddling every 300 s
 *          refresh instant for an hour and the run ends with the last hold. Each pulse (1.2 mm) is
 *          below the initial capacity and drains inside the hold, so the surface takes 1.2 mm of a
 *          6 mm offer. Under OFFER the kernels remember 6 mm per hold and their capacity decays
 *          toward the minimum within a few holds, until a hold can no longer absorb its pulse and
 *          water is left ponded at the end; under APPLIED they remember 1.2 mm per hold and absorb
 *          every pulse. Both close their 2D continuity; APPLIED infiltrates more by the end and
 *          leaves less ponded; the option round-trips through the writer; an unknown word is an
 *          input error. A second deck rains from a gage onto two Horton cells beside two
 *          aquifer-owned cells: the kernel cells keep their own intake (two defects fixed
 *          2026-11-27 had the aquifer zero their held rate and refuse their water), the memory
 *          scheme is exercised there too, and a managed checkpoint session refuses the mesh by name.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_checkpoint.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_infil2d.h>

namespace fs = std::filesystem;

namespace {

// gage_rain: rain from a system gage (two minutes at 180 mm/h straddling every 300 s refresh
// instant for an hour: 6 mm per pulse, more than a hold's Horton capacity can take, so the
// infiltrated total depends on the kernel's history) instead of the API override, and two of the
// four cells owned by a 2D
// aquifer and the junction coupled to the center vertex, so the deck can start a managed
// checkpoint session (which requires the mesh aquifer profile and a coupled or absent 1D
// network) while the other two cells keep their own Horton kernels.
std::string model(const char* memory, bool gage_rain = false) {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\n"
         "FLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\n"
         "START_TIME           00:00:00\n"
         "END_DATE             01/01/2026\n"
         "END_TIME             01:00:00\n"
         "REPORT_STEP          00:05:00\n"
         "WET_STEP             00:05:00\n"
         "DRY_STEP             00:05:00\n"
         "ROUTING_STEP         10\n"
         "ALLOW_PONDING        NO\n"
         "\n[JUNCTIONS]\nJ1      0.0   1.0       0          0         0\n"
         "\n[OUTFALLS]\nO1     -0.5    FREE  NO\n"
         "\n[CONDUITS]\nC1      J1    O1  30.0    0.013      0         0          0\n"
         "\n[XSECTIONS]\nC1      CIRCULAR  0.3    0      0      0      1\n";
    if (gage_rain) {
        s << "\n[RAINGAGES]\nRG1  INTENSITY  0:01  1.0  TIMESERIES  TS1\n"
             "\n[TIMESERIES]\n";
        // An INTENSITY point covers one recording interval, so both minutes of a pulse are
        // listed; the gaps are dry.
        for (int k = 0; k < 12; ++k)
            for (int minute : {5 * k + 4, 5 * k + 5})
                s << "TS1  " << minute / 60 << ':' << (minute % 60 < 10 ? "0" : "") << minute % 60
                  << "  180\n";
    }
    s << "\n[2D_OPTIONS]\n"
         "MAX_TIMESTEP     2\n"
         "DRY_DEPTH        0.001\n"
         "REPORT_2D        NO\n"
         "INFIL_STEP       00:05:00\n"
         "INFIL_MEMORY     "
      << memory << "\n"
      << (gage_rain ? "RAINFALL_MODE    SYSTEM\nGROUNDWATER      YES\n" : "")
      << "\n[2D_VERTICES]\n"
         " 0.0    0.0   10.0\n"
         "20.0    0.0   10.0\n"
         "20.0   20.0   10.0\n"
         " 0.0   20.0   10.0\n"
         "10.0   10.0   10.0\n"
         "\n[2D_TRIANGLES]\n"
         "0     1   4   0.03        0.0         LAWN\n"
         "1     2   4   0.03        0.0         LAWN\n"
      << (gage_rain ? "2     3   4   0.03        0.0         SOIL\n"
                      "3     0   4   0.03        0.0         SOIL\n"
                    : "2     3   4   0.03        0.0         LAWN\n"
                      "3     0   4   0.03        0.0         LAWN\n")
      << "\n[2D_INFILTRATION_DEFAULTS]\n"
         // CMS project: Horton rates in mm/h (3 and 0.5 in/h), decay 1/h, drying time days.
         "LAWN     HORTON     76.2  12.7  4.14  7.0  0    LOST\n";
    if (gage_rain)
        s << "\n[2D_AQUIFER_OPTIONS]\n"
             "MODE MESH\nSOIL_CHAR GARDNER\nCLOSURE CLOSED_FORM\nNODE_ENROLMENT ROWS\n"
             "GW_ET NONE\nGW_CLOCK SHARED\nGW_BASE_STEP .125\nGW_MAX_STEP 8\nLINK_SEEPAGE NONE\n"
             "\n[2D_AQUIFER]\n"
             // SI: Ks mm/h, column thickness m, porosity, residual content, Gardner alpha 1/m.
             // A slow aquifer, so the Horton cells' kernels, not their neighbors, govern.
             "TAG SOIL  0.36  8  .45  .1  2  HG0 2\n"
             // A token coupling: the session profile needs the network coupled, the pulses must stay
             // on the surface to be infiltrated.
             "\n[2D_VERTEX_NODE_MAP]\n;;Vertex  Node  Cd    Area\n4  J1  0.65  0.001\n";
    return s.str();
}

struct Outcome {
    bool ok = false;
    double rainfall_in = 0, infil_out = 0, final_storage = 0, cont_2d = 0;
    std::string written;
};

Outcome runModel(const fs::path& dir, const std::string& name, const std::string& text) {
    Outcome r;
    fs::create_directories(dir);
    const fs::path inp = dir / (name + ".inp"), rpt = dir / (name + ".rpt"),
                   out = dir / (name + ".out"), saved = dir / (name + "_infil_memory_written.inp");
    std::ofstream(inp) << text;
    SWMM_Engine eng = swmm_engine_create();
    if (swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(), out.string().c_str(),
                         nullptr) != SWMM_OK ||
        swmm_engine_initialize(eng) != SWMM_OK) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return r;
    }
    if (swmm_model_write(eng, saved.string().c_str()) == SWMM_OK) {
        std::ifstream f(saved);
        r.written.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return r;
    }
    const bool gage = text.find("[RAINGAGES]") != std::string::npos;
    double elapsed = 0.0;
    for (int guard = 0; guard < 100000; ++guard) {
        // 72 mm/h for one minute straddling each 300 s refresh instant (the refresh falls at
        // 290 s into the cycle), dry otherwise: the held offer is the rain-limited rate for the
        // whole hold while the rain stops after a minute, so each cell takes 1.2 mm of a 6 mm
        // offer and dries inside the hold.
        const double t = elapsed * 86400.0;
        const double rain = (std::fmod(t + 20.0, 300.0) < 60.0 && t < 3600.0) ? 2.0e-5 : 0.0;
        if (!gage)
            swmm_2d_force_rainfall_uniform(eng, rain, SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST);
        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0)
            break;
    }
    swmm_engine_end(eng);
    double init = 0;
    swmm_2d_get_mass_balance(eng, &init, &r.final_storage, &r.rainfall_in, nullptr, nullptr,
                             nullptr, nullptr, nullptr, nullptr, nullptr);
    swmm_infil2d_get_total_volume(eng, &r.infil_out);
    swmm_2d_get_continuity_error(eng, &r.cont_2d);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    r.ok = true;
    return r;
}

// A gage-rain engine attached to a managed checkpoint session (no binary or mesh output).
struct Session {
    SWMM_Engine eng = nullptr;
    std::string error;
    Session(const fs::path& dir, const std::string& name, const char* memory) {
        fs::create_directories(dir);
        const fs::path inp = dir / (name + ".inp"), rpt = dir / (name + ".rpt");
        std::ofstream(inp) << model(memory, true);
        fs::remove(rpt); // a managed session wants a fresh report destination
        eng = swmm_engine_create();
        if (swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(), nullptr, nullptr) !=
            SWMM_OK) {
            error = "open failed";
            return;
        }
        SWMM_CheckpointSessionOptions options{};
        options.struct_size = sizeof options;
        options.schema_version = 1;
        options.output = SWMM_CHECKPOINT_OUTPUT_NONE;
        if (swmm_checkpoint_start_session(eng, &options) != SWMM_OK)
            error = std::string("start_session: ") + swmm_checkpoint_last_error_msg();
    }
    ~Session() {
        swmm_engine_end(eng);
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
    }
};

} // namespace

TEST(InfilMemory, AppliedMemoryClosesAndRemembersOnlyWhatInfiltrated) {
    const fs::path dir = fs::current_path() / "test_2d_infil_memory_artifacts";
    const Outcome offer = runModel(dir, "offer", model("OFFER"));
    const Outcome applied = runModel(dir, "applied", model("APPLIED"));
    ASSERT_TRUE(offer.ok);
    ASSERT_TRUE(applied.ok);
    ASSERT_GT(offer.rainfall_in, 0.0);
    EXPECT_NEAR(applied.rainfall_in, offer.rainfall_in, 1e-9 * offer.rainfall_in);
    EXPECT_LT(std::fabs(offer.cont_2d), 0.1) << "2D continuity error (%) under OFFER";
    EXPECT_LT(std::fabs(applied.cont_2d), 0.1) << "2D continuity error (%) under APPLIED";
    // The offer scheme over-remembers infiltration, presents less capacity later and leaves
    // part of the last pulses ponded when the run ends.
    EXPECT_GT(applied.infil_out - offer.infil_out, 1e-3 * offer.rainfall_in)
        << "APPLIED " << applied.infil_out << " m3, OFFER " << offer.infil_out << " m3 of "
        << offer.rainfall_in << " m3 of rain";
    EXPECT_LT(applied.final_storage, offer.final_storage);
    // APPLIED is the default and is not written; OFFER is.
    EXPECT_EQ(applied.written.find("INFIL_MEMORY"), std::string::npos);
    EXPECT_NE(offer.written.find("INFIL_MEMORY           OFFER"), std::string::npos)
        << offer.written.substr(0, 2500);
}

TEST(InfilMemory, UnknownWordIsAnInputError) {
    const fs::path dir = fs::current_path() / "test_2d_infil_memory_artifacts";
    EXPECT_FALSE(runModel(dir, "bad", model("REMEMBER")).ok);
}

// Two cells run Horton beside two aquifer-owned cells: the aquifer's rate publication must not
// zero the kernel cells' held rate, and the applied-memory scheme must be exercised on this deck
// (otherwise the checkpoint cases below could not tell a lost snapshot from a kept one).
TEST(InfilMemory, KernelCellsKeepTheirRateBesideAnAquifer) {
    const fs::path dir = fs::current_path() / "test_2d_infil_memory_artifacts";
    const Outcome offer = runModel(dir, "mixed_offer", model("OFFER", true));
    const Outcome applied = runModel(dir, "mixed_applied", model("APPLIED", true));
    ASSERT_TRUE(offer.ok);
    ASSERT_TRUE(applied.ok);
    EXPECT_GT(applied.rainfall_in, 0.0);
    EXPECT_GT(applied.infil_out, 1e-3 * applied.rainfall_in)
        << "the Horton cells infiltrated " << applied.infil_out << " m3 of " << applied.rainfall_in
        << " m3 of rain";
    EXPECT_NE(applied.infil_out, offer.infil_out)
        << "APPLIED and OFFER agree on this deck, so a lost memory could not be detected";
}

// A managed checkpoint session awards infiltration through the 2D aquifer only, so a mesh with
// its own infiltration kernels is refused at attachment rather than run with their intake
// dropped. The refusal names the reason. (The INFIL_MEMORY snapshot therefore has no checkpoint
// to live in yet; see the manual's INFIL_MEMORY row.)
TEST(InfilMemory, ManagedCheckpointSessionRefusesKernelCellsByName) {
    const fs::path dir = fs::current_path() / "test_2d_infil_memory_artifacts";
    Session s(dir, "session_refused", "APPLIED");
    EXPECT_NE(s.error.find("aquifer-owned cells"), std::string::npos) << s.error;
}
