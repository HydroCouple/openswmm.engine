// SPDX-License-Identifier: Apache-2.0

/**
 * @file      test_engine_checkpoint.cpp
 * @brief     Tests for engine checkpoint.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include "core/SWMMEngine.hpp"
#include "core/HotStartManager.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "core/CheckpointInspection.hpp"
#include "core/DateTime.hpp"
#include "io/CheckpointDirectory.hpp"
#include "io/CheckpointOutputPublication.hpp"
#include "core/FileIO.hpp"
#include "plugins/DefaultReportPlugin.hpp"
#include <openswmm/engine/openswmm_checkpoint.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_snow.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_heat.h>
#include <openswmm/engine/openswmm_hotstart.h>
#include <openswmm/engine/openswmm_gw_transport.h>
#include "hydrology/surface/SnowCheckpoint.hpp"
#include <atomic>
#include <chrono>
#include <thread>
#include <future>
#include "output/OutputReader.hpp"
#include <openswmm/plugin_sdk/IOutputPlugin.hpp>
#ifdef OPENSWMM_HAS_2D
#include "2d/subsurface/CommonSourceStage.hpp"
#include "hydrology/LidNode.hpp"
#include "2d/subsurface/SourceResultHistory.hpp"
#include "2d/subsurface/EngineSourceResultHistory.hpp"
#include "2d/subsurface/EngineSourceResultStream.hpp"
#include "2d/heat/LidNodeHeat.hpp"
#include "io/CheckpointFile.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include <openswmm/engine/openswmm_coupling.h>
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
#include "2d/output/Default2DOutputPlugin.hpp"
#endif
#endif

#include <filesystem>
#ifndef _WIN32
#include <sys/resource.h>
#include <csignal>
#endif
#include <fstream>
#include <cstdlib>
#include <iterator>
#include <locale>
#include <future>
#include <numeric>
#include <set>

using namespace openswmm;
namespace {
class HeldOutput final : public IOutputPlugin {
public:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    int writes = 0;
    /**
     * @brief Report the blocking fixture plugin as perpetually prepared.
     *
     * This test double does not implement normal plugin lifecycle transitions; no mutable state is
     * read.
     * @return PluginState::PREPARED regardless of initialization or write count.
     */
    PluginState state() const noexcept override { return PluginState::PREPARED; }
    /**
     * @brief Accept fixture initialization without retaining supplied metadata.
     *
     * Both unnamed arguments are deliberately ignored: initialization strings and borrowed component
     * info are neither inspected nor retained. Does not change state or reset writes/released flags.
     * @return Zero unconditionally.
     */
    int initialize(const std::vector<std::string>&, const IPluginComponentInfo*) override {
        return 0;
    }
    /**
     * @brief Accept fixture validate without model access or lifecycle transition.
     *
     * The unnamed borrowed context is ignored and not retained. Does not release blocked update calls,
     * reset write counters or modify the fixed PREPARED state.
     * @return Zero unconditionally.
     */
    int validate(const SimulationContext&) override { return 0; }
    /**
     * @brief Accept fixture prepare without model access or lifecycle transition.
     *
     * The unnamed borrowed context is ignored and not retained. Does not release blocked update calls,
     * reset write counters or modify the fixed PREPARED state.
     * @return Zero unconditionally.
     */
    int prepare(const SimulationContext&) override { return 0; }
    /**
     * @brief Accept fixture finalize without model access or lifecycle transition.
     *
     * The unnamed borrowed context is ignored and not retained. Does not release blocked update calls,
     * reset write counters or modify the fixed PREPARED state.
     * @return Zero unconditionally.
     */
    int finalize(const SimulationContext&) override { return 0; }
    /**
     * @brief Expose the stable diagnostic label of the blocking output test double.
     * @return Borrowed static string held output, valid for process lifetime; caller must not free it.
     */
    const char* last_error_message() const noexcept override { return "held output"; }
    /**
     * @brief Hold an output callback until the test releases it, then inject the second-write failure.
     *
     * Ignores the unnamed snapshot and retains no borrowed data. Locks mutex, sets entered, notifies
     * waiters, then waits until released is true before incrementing writes. Test controller must set
     * release under mutex and notify changed; otherwise callback can block indefinitely. Does not reset
     * released/entered between updates; update state is mutex-protected.
     * @return -17 for the second released update, zero for every other update.
     */
    int update(const SimulationSnapshot&) override {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
        return ++writes == 2 ? -17 : 0;
    }
};
/**
 * @brief Create the session-checkpoint evidence directory with first-call default cleanup.
 *
 * On the first call only, recursively removes the selected default folder when the environment
 * variable was absent, ignoring removal errors; an environment-selected folder is never cleared.
 * Later calls re-evaluate the path but do not repeat cleanup. Creates the returned directory and
 * propagates creation/path errors. Fixed-name artifacts are intentionally retained for review after
 * this run; callers serialize test writes.
 * @return Owned SWMM_R4_SESSION_OUT directory when present, otherwise source-relative
 *     engine_checkpoint_out.
 */
std::filesystem::path root() {
    const auto* env = std::getenv("SWMM_R4_SESSION_OUT");
    auto p = env ? std::filesystem::path(env)
                 : std::filesystem::path(__FILE__).parent_path() / "engine_checkpoint_out";
    // The tests write fixed names here and some refuse an existing
    // destination, so a previous run's files made them fail. The default
    // folder starts empty once per run and keeps this run's files for review;
    // a folder given in SWMM_R4_SESSION_OUT is never emptied.
    static const bool emptied = [&] {
        if (!env) {
            std::error_code ec;
            std::filesystem::remove_all(p, ec);
        }
        return true;
    }();
    (void)emptied;
    std::filesystem::create_directories(p);
    return p;
}
/**
 * @brief Read complete checkpoint evidence into owned binary text.
 *
 * Does not assert IO status or parse archives; local stream closes at return and allocation
 * exceptions propagate.
 *
 * @param[in] p Borrowed file path opened in binary mode.
 * @return Owned full/partial bytes, empty for empty/unopenable input.
 */
std::string bytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
/**
 * @brief Overwrite checkpoint fixture evidence with caller-supplied bytes.
 *
 * Uses a binary truncating stream and ignores open/write/close success. Does not parse, escape or
 * append a newline; caller chooses paths safe to replace.
 *
 * @param[in] p Borrowed destination path; parent directory must already exist.
 * @param[in] data Borrowed string copied in full, including embedded NUL bytes.
 */
void write(const std::filesystem::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary);
    out << data;
}
/**
 * @brief Author and start the baseline 1D session-checkpoint fixture.
 *
 * Builds CFS dynamic-wave storage/conduit geometry across midnight, 0.7 s routing and three-second
 * reporting, with optional native LID cells. Writes unchecked input then throws runtime_error on
 * open/init/start/checkpoint rejection. Starts without saved output; caller owns all engine and
 * later lifecycle cleanup. Raw selectors/name are not independently validated.
 *
 * @param[in,out] e Caller-owned engine suitable for open/init/start; left running on success or
 *     partially mutated before an exception.
 * @param[in] name Raw artifact stem below root; matching .inp/.rpt can be overwritten and an
 *     average- prefix enables report averaging.
 * @param[in] lid 0 plain storage, 1 NODE LID, >=2 Richards LID, 3 additionally creates a second
 *     LID/backwater outlet.
 * @param[in] schedule 0 continuous forcing, 1 installs two activity windows, 2 authors a dry
 *     steady-skip profile.
 * @param[in] controls Append competing depth/time conduit control rules when true.
 * @param[in] checkpoint Enable session checkpointing after start when true.
 * @param[in] surcharge Native surcharge method assigned before initialization.
 */
void start(SWMMEngine& e, const std::string& name, int lid = 0, int schedule = 0,
           bool controls = true, bool checkpoint = true, int surcharge = 0) {
    auto p = root() / name;
    std::string deck =
        "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/31/2004\nSTART_TIME 23:59:00\nEND_DATE 02/01/2004\nEND_TIME 00:02:00\nROUTING_STEP .7\nMINIMUM_STEP .001\nVARIABLE_STEP .75\nRULE_STEP 00:00:02\nWET_STEP 00:00:10\nDRY_STEP 00:00:20\nREPORT_STEP 00:00:03\nIGNORE_QUALITY YES\nIGNORE_2D YES\nSKIP_STEADY_STATE ";
    deck += schedule == 2 ? "YES\n" : "NO\n";
    deck +=
        "[STORAGE]\nS 0 3 1 FUNCTIONAL 0 0 100 0 .5 6 1 .3\n[OUTFALLS]\nO 0 FIXED .4 NO\n[CONDUITS]\nC S O 30 .013 .75 0 0 0\n[XSECTIONS]\nC CIRCULAR .3 0 0 0\n[EVAPORATION]\nMONTHLY .1 .2 .1 .1 .1 .1 .1 .1 .1 .1 .1 .1\n";
    if (!lid && schedule != 2)
        deck.replace(deck.find("FUNCTIONAL 0 0 100"), std::string("FUNCTIONAL 0 0 100").size(),
                     "FUNCTIONAL 20 1 100");
    if (lid)
        deck.replace(deck.find("S 0 3 1 FUNCTIONAL 0 0 100 0 .5 6 1 .3"),
                     std::string("S 0 3 1 FUNCTIONAL 0 0 100 0 .5 6 1 .3").size(),
                     "S 0 2 1 FUNCTIONAL 0 0 100 0 0");
    if (schedule == 2 && !lid) {
        deck.replace(deck.find("S 0 3 1"), 7, "S 0 3 0");
        deck.replace(deck.find("0 .5 6 1 .3"), 10, "0 0");
        deck.replace(deck.find("FIXED .4"), 8, "FIXED 0");
        deck.replace(deck.find("MONTHLY .1 .2 .1 .1 .1 .1 .1 .1 .1 .1 .1 .1"),
                     std::string("MONTHLY .1 .2 .1 .1 .1 .1 .1 .1 .1 .1 .1 .1").size(),
                     "CONSTANT 0");
    }
    if (schedule != 2)
        deck +=
            "[INFLOWS]\nS FLOW Q FLOW 1 1 0\n[TIMESERIES]\nQ 01/31/2004 23:59:00 .05\nQ 01/31/2004 23:59:27 -.02\nQ 02/01/2004 00:00:31 .025\nQ 02/01/2004 00:02:00 .03\n[DWF]\nS FLOW .005\n";
    if (controls)
        deck +=
            "[CONTROLS]\nRULE CLOSED_RULE\nIF NODE S DEPTH > .2\nTHEN CONDUIT C STATUS = OFF\nELSE CONDUIT C STATUS = ON\nPRIORITY 1\nRULE OPEN_RULE\nIF SIMULATION TIME > 00:01:00\nTHEN CONDUIT C STATUS = ON\nPRIORITY 2\n";
    if (lid) {
        deck +=
            "[LID_CONTROLS]\nStack NODE\nStack SURFACE 6 .1\nStack MEDIA 12 .45 .2 .08 2 10 3\nStack AGGREGATE 6 .4 100\nStack BOTTOM .5 0\n[LID_NODES]\nS Stack 10\n";
        if (lid >= 2)
            deck +=
                "[LID_RICHARDS]\nStack OPTIONS 4 1e-7 1e-5 30\nStack 2 .03 2 1.6 .5 .0001\nStack 3 .03 2 1.6 .5 .0001\n";
    }
    if (lid == 3)
        deck +=
            "[STORAGE]\nS2 0 2 1 FUNCTIONAL 0 0 80 0 0\n[CONDUITS]\nC2 S2 O2 30 .013 .5 0 0 0\n[XSECTIONS]\nC2 CIRCULAR .3 0 0 0\n[LID_NODES]\nS2 Stack 20\n[OUTFALLS]\nO2 0 FIXED 1.25 NO\n";
    write(p.string() + ".inp", deck);
    auto error = e.open((p.string() + ".inp").c_str(), (p.string() + ".rpt").c_str(), nullptr);
    if (error)
        throw std::runtime_error("open " + name + ": " + std::to_string(error) + " " +
                                 e.context().error_message + " " +
                                 (e.context().errors.empty() ? "" : e.context().errors.front()));
    if (schedule == 1) {
        double date = e.context().options.start_date;
        e.context().events = {{datetime::addSeconds(date, 15), datetime::addSeconds(date, 55)},
                              {datetime::addSeconds(date, 85), datetime::addSeconds(date, 140)}};
    }
    e.context().options.surcharge_method = surcharge;
    if (name.starts_with("average-"))
        e.context().options.rpt_averages = true;
    if (e.initialize() || e.start(0))
        throw std::runtime_error("initialize/start " + name + ": " + e.context().error_message +
                                 " " +
                                 (e.context().errors.empty() ? "" : e.context().errors.front()));
    if (checkpoint) {
        auto result = e.enableCheckpointing();
        if (!result.empty())
            throw std::runtime_error(result);
    }
}
// Mixed links exercise the engine-owned callback, sequential shared-well pumps,
// PID history, slow orifice motion and current LID cell-port/backwater heads.
/**
 * @brief Author and start mixed structures with PID and optional node-LID checkpoint state.
 *
 * Creates shared-well pumps, orifice/weir/outlet/conduit and control histories using CFS authoring.
 * Odd kinds install event windows; kind 5 uses a depth-independent pump source. Input writes are
 * unchecked; open/init/start/checkpoint errors throw with partial engine state retained for caller
 * cleanup. Requires known selector bounds for array indexing and physical fixture values; no
 * rollback occurs.
 *
 * @param[in,out] e Caller-owned openable engine; started without saved results on success.
 * @param[in] name Raw evidence stem below root, used for input/report artifacts.
 * @param[in] mode Native surcharge method assigned before initialize.
 * @param[in] lid 0 ordinary storage, nonzero NODE LID, >=2 Richards flow, 3 adds second
 *     LID/backwater storage.
 * @param[in] kind Nonnegative structure-family selector 0..5; determines pump curve/type,
 *     orifice/outlet parity, weir shape and optional source storage.
 * @param[in] checkpoint Enable session checkpointing after start when true.
 * @param[in] cd Raw orifice discharge coefficient copied into the input.
 */
void startStructures(SWMMEngine& e, const std::string& name, int mode = 0, int lid = 0,
                     int kind = 0, bool checkpoint = true, double cd = .65) {
    auto p = root() / name;
    std::string deck =
        "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/31/2004\nSTART_TIME 23:59:00\nEND_DATE 02/01/2004\nEND_TIME 00:01:00\nROUTING_STEP .7\nMINIMUM_STEP .001\nVARIABLE_STEP .75\nRULE_STEP 00:00:02\nREPORT_STEP 00:00:03\nIGNORE_QUALITY YES\nIGNORE_2D YES\nSKIP_STEADY_STATE NO\n";
    deck +=
        "[STORAGE]\nS 0 3 1.4 FUNCTIONAL 0 0 100 0 .2 6 1 .3\n[JUNCTIONS]\nJ 0 3 .1 0 0\n[OUTFALLS]\nO 0 FIXED .8 NO\nOR 0 FIXED .8 NO\nOW 0 FIXED .8 NO\nOX 0 FIXED .8 NO\n[CONDUITS]\nC J O 40 .013 0 0 0 0\n[PUMPS]\n";
    deck += kind == 5 ? "P T J " : "P S J ";
    deck += kind == 5 ? "*" : "PC";
    deck += " ON .9 .3\nP2 S J PC2 ON 1.1 .4\n[ORIFICES]\nR S OR ";
    deck += kind % 2 ? "SIDE" : "BOTTOM";
    deck += " .6 " + std::to_string(cd) + " NO .01\n[WEIRS]\nW S OW ";
    const char* kinds[] = {"TRANSVERSE", "SIDEFLOW", "V-NOTCH", "TRAPEZOIDAL", "ROADWAY"};
    deck += kinds[kind % 5];
    deck += " 1.05 3.33 NO 0 0 YES";
    if (kind % 5 == 4)
        deck += " 8 PAVED";
    deck += "\n";
    deck += "[OUTLETS]\nX S OX .5 ";
    deck += kind % 2 ? "TABULAR/HEAD XC" : "FUNCTIONAL/DEPTH .04 .5";
    deck += " NO\n[XSECTIONS]\nC CIRCULAR .5 0 0 0\nR CIRCULAR .2 0 0 0\nW ";
    deck += kind % 5 == 2 ? "TRIANGULAR 1 60 0 0"
                          : (kind % 5 == 3 ? "TRAPEZOIDAL 1 .3 1 1" : "RECT_OPEN 1 .3 0 0");
    deck += "\n[CURVES]\nXC RATING 0 0\nXC 3 .1\nPC2 PUMP4 0 0\nPC2 .8 .025\nPC2 3 .08\n";
    if (kind < 5) {
        deck += "PC PUMP" + std::to_string(kind + 1);
        // The stepped PUMP2 curve must leave water for P2 at their shared
        // Richards port; the larger continuous-curve rates starve P2 here.
        if (kind == 1)
            deck += " 0 0\nPC .8 .0025\nPC 3 .008\n";
        else
            deck += kind == 0 ? " 0 .01\nPC 50 .04\nPC 300 .08\n"
                              : (kind == 2 ? " 0 .08\nPC 1 .04\nPC 3 0\n"
                                           : " 0 0\nPC .8 .025\nPC 3 .08\n");
    }
    deck +=
        "[EVAPORATION]\nMONTHLY .1 .2 .1 .1 .1 .1 .1 .1 .1 .1 .1 .1\n[INFLOWS]\nS FLOW Q FLOW 1 1 .02\n[TIMESERIES]\nQ 01/31/2004 23:59:00 .02\nQ 01/31/2004 23:59:35 .09\nQ 02/01/2004 00:00:30 -.01\nQ 02/01/2004 00:01:00 .07\nA 01/31/2004 23:59:00 .2\nA 02/01/2004 00:01:00 .9\n";
    deck +=
        "[CONTROLS]\nRULE P_PID\nIF NODE S DEPTH <> 1.2\nTHEN PUMP P SETTING = PID -.1 .5 .005\nRULE R_PID\nIF NODE S DEPTH <> 1.1\nTHEN ORIFICE R SETTING = PID -.1 .5 .005\nRULE W_TS\nIF NODE S DEPTH > 0\nTHEN WEIR W SETTING = TIMESERIES A\nRULE X_NUM\nIF SIMULATION TIME > 00:00:50\nTHEN OUTLET X SETTING = .4\nELSE OUTLET X SETTING = .8\nRULE R_OFF\nIF SIMULATION TIME > 00:01:10\nAND SIMULATION TIME < 00:01:25\nTHEN ORIFICE R SETTING = 0\nPRIORITY 2\n";
    if (kind == 5)
        deck +=
            "[STORAGE]\nT 0 3 1.4 FUNCTIONAL 0 0 100 0 0\n[INFLOWS]\nT FLOW \"\" FLOW 1 1 .015\n";
    if (lid) {
        deck.replace(deck.find("S 0 3 1.4 FUNCTIONAL 0 0 100 0 .2 6 1 .3"),
                     std::string("S 0 3 1.4 FUNCTIONAL 0 0 100 0 .2 6 1 .3").size(),
                     "S 0 2 1.4 FUNCTIONAL 0 0 100 0 .2");
        deck +=
            "[LID_CONTROLS]\nStack NODE\nStack SURFACE 6 .1\nStack MEDIA 12 .45 .2 .08 2 10 3\nStack AGGREGATE 6 .4 100\nStack BOTTOM .5 0\n[LID_NODES]\nS Stack 25\n";
        if (lid >= 2)
            deck +=
                "[LID_RICHARDS]\nStack OPTIONS 4 1e-7 1e-5 30\nStack 2 .03 2 1.6 .5 .0001\nStack 3 .03 2 1.6 .5 .0001\n";
        if (lid == 3)
            deck +=
                "[STORAGE]\nS2 0 2 1 FUNCTIONAL 0 0 80 0 0\n[CONDUITS]\nC2 S2 O2 30 .013 .5 0 0 0\n[XSECTIONS]\nC2 CIRCULAR .3 0 0 0\n[LID_NODES]\nS2 Stack 20\n[OUTFALLS]\nO2 0 FIXED 1.25 NO\n";
    }
    write(p.string() + ".inp", deck);
    if (e.open((p.string() + ".inp").c_str(), (p.string() + ".rpt").c_str(), nullptr))
        throw std::runtime_error("structure open: " + e.context().error_message + " " +
                                 (e.context().errors.empty() ? "" : e.context().errors.front()));
    e.context().options.surcharge_method = mode;
    if (kind % 2) {
        auto date = e.context().options.start_date;
        e.context().events = {{datetime::addSeconds(date, 5), datetime::addSeconds(date, 40)},
                              {datetime::addSeconds(date, 60), datetime::addSeconds(date, 110)}};
    }
    if (e.initialize() || e.start(0))
        throw std::runtime_error("structure start: " + e.context().error_message);
    if (checkpoint) {
        auto error = e.enableCheckpointing();
        if (!error.empty())
            throw std::runtime_error(error);
    }
}
// Rainfall, upstream run-on, outfall feedback and all eight legacy LID types
// share the actual engine scheduler/structure/node-LID pipeline.
/**
 * @brief Author and start a coupled runoff/LID scheduler checkpoint fixture.
 *
 * Uses a temporary authored engine, reopens its rewritten CFS deck with three catchments, all eight
 * native LID types, gage/subcatchment multipliers, monthly adjustments, runon and outfall return
 * routing. Selectors index fixed arrays and require the documented bounds. Unchecked writes or
 * failed exact deck anchors can leave invalid input; lifecycle/checkpoint rejections throw with
 * partial caller engine state retained. Starts without saved results; caller owns cleanup.
 *
 * @param[in,out] e Caller-owned engine suitable for open/init/start; left
 *     running/checkpoint-enabled on success.
 * @param[in] name Raw artifact stem below root; matching input/report paths are overwritten.
 * @param[in] method Infiltration selector 0..4: Horton, modified Horton, Green-Ampt, modified
 *     Green-Ampt, curve number.
 * @param[in] rain Rain record format selector 0 intensity, 1 volume, 2 cumulative.
 * @param[in] node_lid NODE LID mode forwarded to the mixed-structure base fixture.
 * @param[in] checkpoint Enable session checkpointing after start when true.
 */
void startRunoff(SWMMEngine& e, const std::string& name, int method = 0, int rain = 0,
                 int node_lid = 0, bool checkpoint = true) {
    SWMMEngine authored;
    startStructures(authored, name, method % 4, node_lid, 0, false);
    if (authored.close())
        throw std::runtime_error("runoff fixture close failed");
    auto p = root() / name;
    auto deck = bytes(p.string() + ".inp");
    deck.replace(deck.find("END_TIME 00:01:00"), std::string("END_TIME 00:01:00").size(),
                 "END_TIME 00:03:00");
    deck +=
        "[OPTIONS]\nIGNORE_SNOWMELT YES\nIGNORE_GROUNDWATER YES\nWET_STEP 00:00:07\nDRY_STEP 00:00:23\n";
    const char* methods[] = {"HORTON", "MODIFIED_HORTON", "GREEN_AMPT", "MODIFIED_GREEN_AMPT",
                             "CURVE_NUMBER"};
    deck += "INFILTRATION " + std::string(methods[method]) + "\n[RAINGAGES]\n";
    const char* formats[] = {"INTENSITY", "VOLUME", "CUMULATIVE"};
    for (const char* g : {"R1", "R2", "Unused"})
        deck += std::string(g) + " " + formats[rain] + " 0:01 1 TIMESERIES Rain\n";
    deck +=
        "[SUBCATCHMENTS]\nA R1 B .05 45 30 1 0\nB R2 S .08 65 40 2 0\nD R1 S .06 35 35 1 0\n[SUBAREAS]\nA .015 .12 .03 .05 35 PERVIOUS 55\nB .02 .13 .02 .04 25 IMPERVIOUS 45\nD .02 .13 .02 .04 25 OUTLET\n[INFILTRATION]\n";
    for (const char* sub : {"A", "B", "D"})
        deck += std::string(sub) +
                (method < 2 ? " 3 .2 4 7 2\n" : (method < 4 ? " 3 .25 .3\n" : " 75 .25 7\n"));
    deck += "[LID_CONTROLS]\n";
    for (const char* type : {"BC", "RG", "GR", "IT", "PP", "RB", "RD", "VS"}) {
        auto n = std::string("U") + type;
        deck += n + " " + type + "\n" + n + " SURFACE 3 .05 .12 1 3\n";
        if (n == "UBC" || n == "URG" || n == "UGR" || n == "UPP")
            deck += n + " SOIL 6 .45 .2 .08 1 10 3\n";
        if (n == "UPP")
            deck += n + " PAVEMENT 3 .2 0 1 0 0 0\n";
        if (n == "UBC" || n == "UIT" || n == "UPP" || n == "URB")
            deck += n + " STORAGE 6 .667 .1 0\n";
        if (n == "UGR")
            deck += n + " DRAINMAT 2 .5 .1\n";
        if (n == "UBC" || n == "UIT" || n == "UPP" || n == "URB")
            deck += n + " DRAIN .5 .5 0 0\n";
        if (n == "URD")
            deck += n + " DRAIN 2 0 0 0\n";
    }
    deck +=
        "[LID_USAGE]\nA UBC 2 20 3 70 30 1 * B\nA URG 1 20 3 50 30 1\nA UGR 1 20 3 50 20 0 * S\nB UIT 1 20 3 70 20 0 * S\nB UPP 1 20 3 70 20 0 * S\nB URB 1 20 3 70 20 0 * A\nD URD 1 20 3 0 30 1\nD UVS 1 20 3 0 30 0\n";
    deck += "[TIMESERIES]\nRain 01/31/2004 23:59:12 ";
    deck += rain == 0 ? "5" : ".08";
    deck += "\nRain 02/01/2004 00:00:12 ";
    deck += rain == 2 ? ".14" : (rain == 0 ? "3" : ".05");
    deck += "\nRain 02/01/2004 00:01:12 ";
    deck += rain == 2 ? ".14" : "0";
    deck += "\nRain 02/01/2004 00:02:12 ";
    deck += rain == 0 ? "4" : ".04";
    deck +=
        "\n[PATTERNS]\nMonth MONTHLY .8 1.2 1 1 1 1 1 1 1 1 1 1\n[ADJUSTMENTS]\nRAIN .9 1.2 1 1 1 1 1 1 1 1 1 1\nINFIL A Month\nN-PERV B Month\nDSTORE D Month\n";
    write(p.string() + ".inp", deck);
    if (e.open((p.string() + ".inp").c_str(), (p.string() + ".rpt").c_str(), nullptr))
        throw std::runtime_error("runoff open: " + e.context().error_message + " " +
                                 (e.context().errors.empty() ? "" : e.context().errors.front()));
    e.context().gages.scale_factor[1] = 1.3;
    e.context().subcatches.rain_scale_factor[1] = .8;
    e.context().node_subtypes.outfalls.route_to[0] = 2;
    if (e.initialize() || e.start(0))
        throw std::runtime_error("runoff start: " + e.context().error_message + " " +
                                 (e.context().errors.empty() ? "" : e.context().errors.front()));
    if (checkpoint) {
        auto error = e.enableCheckpointing();
        if (!error.empty())
            throw std::runtime_error(error);
    }
}
/**
 * @brief Advance one checkpoint fixture engine step or throw its diagnostic.
 *
 * Throws runtime_error with current elapsed seconds and context error_message on rejected step.
 * Does not inspect returned elapsed completion, enforce progress or close the engine; caller
 * chooses a reachable stop condition.
 *
 * @param[in,out] e Caller-owned started engine advanced in place.
 */
void step(SWMMEngine& e) {
    double t = -1;
    auto result = e.step(&t);
    if (result)
        throw std::runtime_error("step at " + std::to_string(e.context().current_time) + ": " +
                                 e.context().error_message);
}
/**
 * @brief Extract physical session bytes from known output/source-stream checkpoint wrappers.
 *
 * Decodes the leading magic and exact recognized wrapper layout through CompletedSourceArchive;
 * malformed/trailing bytes throw. Does not independently validate wrapper checksums or alter the
 * extracted physical payload.
 *
 * @param[in] bytes Owned-by-value complete session archive bytes, not a filename.
 * @return Owned physical checkpoint member for recognized file-backed/source-stream wrappers,
 *     otherwise original bytes.
 */
std::string physicalCheckpoint(std::string bytes) {
    detail::CompletedSourceArchive peek(bytes);
    std::string magic;
    peek(magic);
    if (magic == "OpenSWMM file-backed water engine session") {
        detail::CompletedSourceArchive in(std::move(bytes));
        uint32_t version, checksum;
        std::string physical, binary, mesh, source;
        in(magic, version, checksum, physical, binary, mesh, source);
        in.finish();
        return physical;
    }
    if (magic != "OpenSWMM engine session with source stream")
        return bytes;
    detail::CompletedSourceArchive in(std::move(bytes));
    uint32_t version, checksum;
    std::string physical, contract;
    in(magic, version, checksum, physical, contract);
    in.finish();
    return physical;
}
/**
 * @brief Save two completed engine boundaries and compare physical checkpoint bytes.
 *
 * Fatally requires successful saves and exact physicalCheckpoint equality. Output/source-stream
 * wrapper details are excluded by physicalCheckpoint, so this compares physical continuation rather
 * than every output artifact contract. Files may be overwritten and archive/IO exceptions can
 * propagate.
 *
 * @param[in,out] a Reference engine whose checkpoint state is saved without stepping.
 * @param[in,out] b Candidate engine at the expected same physical boundary.
 * @param[in] name Raw evidence stem below root for -a.chk/-b.chk destinations.
 */
void compare(SWMMEngine& a, SWMMEngine& b, const std::string& name) {
    auto pa = root() / (name + "-a.chk"), pb = root() / (name + "-b.chk");
    ASSERT_EQ(a.saveCheckpoint(pa.string()), "");
    ASSERT_EQ(b.saveCheckpoint(pb.string()), "");
    ASSERT_EQ(physicalCheckpoint(bytes(pa)), physicalCheckpoint(bytes(pb)));
}
/**
 * @brief Compute reflected CRC-32 for synthetic checkpoint re-encoding.
 *
 * No archive parsing or mutation occurs.
 *
 * @param[in] bytes Borrowed payload bytes interpreted as unsigned values.
 * @return CRC-32 with polynomial 0xedb88320 and initial/final complement.
 */
uint32_t crc(const std::string& bytes) {
    uint32_t c = ~uint32_t(0);
    for (unsigned char v : bytes) {
        c ^= v;
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0u);
    }
    return ~c;
}
}

TEST(EngineCheckpoint, ActualEngineContinuationPreservesClocksControlsExfiltrationAndLedgers) {
    for (int mode = 0; mode < 4; ++mode)
        for (int lid = 0; lid < 4; ++lid)
            for (int schedule = 0; schedule < 3; ++schedule) {
                auto name = "continuation-" + std::to_string(mode) + "-" + std::to_string(lid) +
                            "-" + std::to_string(schedule);
                SCOPED_TRACE(name);
                SWMMEngine a, b;
                ASSERT_NO_THROW(start(a, name + "-a", lid, schedule, schedule != 2, true, mode));
                ASSERT_NO_THROW(start(b, name + "-b", lid, schedule, schedule != 2, true, mode));
                const double at = schedule == 1 ? 60 : 40;
                while (a.context().current_time < at)
                    ASSERT_NO_THROW(step(a));
                if (schedule != 2)
                    EXPECT_EQ(a.context().links.target_setting[0], 0);
                if (schedule == 0)
                    EXPECT_GT(a.context().mass_balance.step_ext_withdrawal, 0);
                auto p = root() / (name + ".chk");
                ASSERT_EQ(a.saveCheckpoint(p.string()), "");
                ASSERT_EQ(b.loadCheckpoint(p.string()), "");
                compare(a, b, name);
                int count = 0;
                while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
                    ASSERT_NO_THROW(step(a));
                    ASSERT_NO_THROW(step(b));
                    ASSERT_EQ(a.context().current_time, b.context().current_time);
                    if (++count % 13 == 0)
                        compare(a, b, name);
                }
                compare(a, b, name);
                if (lid == 0 && schedule == 0)
                    EXPECT_GT(a.context().mass_balance.routing_seep_loss, 0);
                if (schedule == 2 && lid == 0)
                    EXPECT_GT(a.context().routing_stats.steady_time, 0);
                if (schedule != 2)
                    EXPECT_EQ(a.context().links.target_setting[0], 1);
                if (lid > 0)
                    EXPECT_EQ(a.context().routing_stats.steady_time, 0);
                EXPECT_GT(a.context().routing_stats.report_steps, 0);
                ASSERT_EQ(a.end(), 0);
                ASSERT_EQ(b.end(), 0);
                EXPECT_EQ(a.context().mass_balance.routing_error(),
                          b.context().mass_balance.routing_error());
                EXPECT_EQ(a.context().routing_stats.computed_avg_iterations(),
                          b.context().routing_stats.computed_avg_iterations());
            }
}

TEST(EngineCheckpoint, InitialAndAveragingBoundariesAndFreshOnlyRestore) {
    SWMMEngine a, b;
    ASSERT_NO_THROW(start(a, "initial-a"));
    ASSERT_NO_THROW(start(b, "initial-b"));
    auto p = root() / "initial.chk";
    ASSERT_EQ(a.saveCheckpoint(p.string()), "");
    ASSERT_EQ(b.loadCheckpoint(p.string()), "");
    compare(a, b, "initial");
    ASSERT_NO_THROW(step(a));
    ASSERT_NO_THROW(step(b));
    compare(a, b, "initial-step");
    EXPECT_FALSE(b.loadCheckpoint(p.string()).empty());
    SWMMEngine c, d;
    ASSERT_NO_THROW(start(c, "average-c", 0, 0, true, false));
    ASSERT_NO_THROW(start(d, "average-d", 0, 0, true, false));
    ASSERT_EQ(c.enableCheckpointing(), "");
    ASSERT_EQ(d.enableCheckpointing(), "");
    for (int i = 0; i < 27; ++i)
        ASSERT_NO_THROW(step(c));
    auto q = root() / "average.chk";
    ASSERT_EQ(c.saveCheckpoint(q.string()), "");
    ASSERT_EQ(d.loadCheckpoint(q.string()), "");
    for (int i = 0; i < 45; ++i) {
        ASSERT_NO_THROW(step(c));
        ASSERT_NO_THROW(step(d));
    }
    compare(c, d, "average");
}

TEST(EngineCheckpoint, CorruptionAndModelMismatchLeaveFreshTargetUnchangedWithValidRetry) {
    SWMMEngine a;
    ASSERT_NO_THROW(start(a, "atomic-source"));
    for (int i = 0; i < 50; ++i)
        ASSERT_NO_THROW(step(a));
    auto good = root() / "atomic-good.chk";
    ASSERT_EQ(a.saveCheckpoint(good.string()), "");
    auto valid = bytes(good);
    detail::CompletedSourceArchive input(valid);
    std::string magic, model, payload;
    uint32_t version, checksum;
    input(magic, version, model, checksum, payload);
    input.finish();
    for (int variant = 0; variant < 13; ++variant) {
        auto name = "atomic-" + std::to_string(variant);
        SCOPED_TRACE(name);
        SWMMEngine b;
        ASSERT_NO_THROW(start(b, name));
        auto before = root() / (name + "-before.chk"), after = root() / (name + "-after.chk"),
             bad = root() / (name + "-bad.chk");
        ASSERT_EQ(b.saveCheckpoint(before.string()), "");
        std::string mutant = valid;
        if (variant == 0)
            mutant.resize(mutant.size() / 2);
        else if (variant == 1)
            mutant.back() ^= 1;
        else if (variant == 2)
            mutant += "extra";
        else {
            auto data = payload;
            auto v = version;
            if (variant == 3)
                v = 99;
            if (variant == 4) {
                double nan = std::numeric_limits<double>::quiet_NaN();
                std::memcpy(data.data(), &nan, sizeof(nan));
            }
            if (variant == 5) {
                double mismatch = 1;
                std::memcpy(data.data() + sizeof(double), &mismatch, sizeof(mismatch));
            }
            if (variant == 6) {
                double future = 1e15;
                std::memcpy(data.data() + 9 * sizeof(double), &future, sizeof(future));
            }
            if (variant == 7)
                data.resize(data.size() - 1);
            if (variant == 8)
                data += "extra";
            if (variant == 9)
                data[7 * sizeof(double) + 6 * sizeof(double) + sizeof(int)] = 2; // invalid bool
            if (variant == 10) {
                int event = -1;
                std::memcpy(data.data() + 13 * sizeof(double), &event, sizeof(event));
            }
            if (variant == 11) {
                double future = 1e15;
                std::memcpy(data.data() + 12 * sizeof(double), &future, sizeof(future));
            }
            if (variant == 12) {
                // Explicit v1 layout through the node-sized zero LID-drain row.
                const auto offset = 7 * sizeof(double) + 6 * sizeof(double) + 2 * sizeof(int) +
                                    4 * sizeof(bool) + 2 * sizeof(double) + 11 * sizeof(double) +
                                    sizeof(uint64_t) + a.context().n_nodes() * sizeof(double) +
                                    23 * sizeof(double);
                double stored;
                std::memcpy(&stored, data.data() + offset, sizeof(stored));
                ASSERT_EQ(stored, a.context().mass_balance.routing_final_storage);
                stored += 1;
                std::memcpy(data.data() + offset, &stored, sizeof(stored));
            }
            auto sum = crc(data);
            detail::CompletedSourceArchive out;
            out(magic, v, model, sum, data);
            mutant = out.bytes();
        }
        write(bad, mutant);
        EXPECT_FALSE(b.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(b.saveCheckpoint(after.string()), "");
        EXPECT_EQ(bytes(before), bytes(after));
        ASSERT_EQ(b.loadCheckpoint(good.string()), "");
        compare(a, b, name + "-retry");
    }
    SWMMEngine c;
    ASSERT_NO_THROW(start(c, "mismatched", 0, 1));
    EXPECT_FALSE(c.loadCheckpoint(good.string()).empty());
}

TEST(EngineCheckpoint, RefusesIncompleteProfilesChangedModelsAndUnrelatedDestinations) {
    SWMMEngine a;
    ASSERT_NO_THROW(start(a, "guards"));
    EXPECT_FALSE(a.enableCheckpointing().empty());
    auto file = root() / "unrelated.txt";
    write(file, "keep this file");
    EXPECT_FALSE(a.saveCheckpoint(file.string()).empty());
    EXPECT_EQ(bytes(file), "keep this file");
    a.context().options.rule_step += 1;
    auto p = root() / "changed.chk";
    EXPECT_FALSE(a.saveCheckpoint(p.string()).empty());
    EXPECT_NE(a.step(nullptr), 0);
    EXPECT_FALSE(a.saveCheckpoint(p.string()).empty());
    SWMMEngine b;
    ASSERT_NO_THROW(start(b, "unsupported", 0, 0, false, false));
    b.context().options.temp_source = 2;
    EXPECT_FALSE(b.enableCheckpointing().empty());
    b.context().options.temp_source = 0;
    b.context().tables[0].is_file_based = true;
    EXPECT_FALSE(b.enableCheckpointing().empty());
    b.context().tables[0].is_file_based = false;
    b.context().forcing.climate_evap_mode = ForcingMode::OVERRIDE;
    EXPECT_FALSE(b.enableCheckpointing().empty());
    b.context().forcing.climate_evap_mode = ForcingMode::NONE;
    ASSERT_EQ(b.enableCheckpointing(), "");
    ASSERT_EQ(b.close(), 0);
    ASSERT_NO_THROW(start(b, "guards-reopened"));
    ASSERT_NO_THROW(step(b));
    EXPECT_EQ(b.saveCheckpoint((root() / "reopened.chk").string()), "");
}

TEST(EngineCheckpoint, StructuralPIDContinuationUsesRealEngineStepsAndCurrentLIDPorts) {
    for (int mode = 0; mode < 4; ++mode)
        for (int lid = 0; lid < 4; ++lid)
            for (int kind = 0; kind < 6; ++kind) {
                auto name = "structural-" + std::to_string(mode) + "-" + std::to_string(lid) + "-" +
                            std::to_string(kind);
                SCOPED_TRACE(name);
                SWMMEngine a, b;
                ASSERT_NO_THROW(startStructures(a, name + "-a", mode, lid, kind));
                ASSERT_NO_THROW(startStructures(b, name + "-b", mode, lid, kind));
                while (a.context().current_time < 47)
                    ASSERT_NO_THROW(step(a));
                const int p2 = a.context().link_names.find("P2");
                ASSERT_GE(p2, 0);
                const double pumped_before = a.context().links.stat_pump_volume[p2];
                EXPECT_GT(pumped_before, 0);
                auto p = root() / (name + ".chk");
                ASSERT_EQ(a.saveCheckpoint(p.string()), "");
                ASSERT_EQ(b.loadCheckpoint(p.string()), "");
                compare(a, b, name);
                int count = 0;
                bool moving = false, flowing = false;
                while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
                    ASSERT_NO_THROW(step(a));
                    ASSERT_NO_THROW(step(b));
                    for (int j = 0; j < a.context().n_links(); ++j) {
                        flowing |= a.context().links.flow[j] != 0;
                        moving |=
                            a.context().links.setting[j] != a.context().links.target_setting[j];
                    }
                    if (++count % 17 == 0)
                        compare(a, b, name);
                }
                compare(a, b, name);
                EXPECT_TRUE(flowing);
                EXPECT_TRUE(moving);
                EXPECT_GE(a.context().links.stat_pump_volume[p2], pumped_before);
                // The reduced PUMP2 fixture sustains P2 across the saved boundary;
                // other curves can exhaust the shared port earlier in the run.
                if (kind == 1)
                    EXPECT_GT(a.context().links.stat_pump_volume[p2], pumped_before);
                EXPECT_EQ(a.context().links.stat_pump_volume[p2],
                          b.context().links.stat_pump_volume[p2]);
                ASSERT_EQ(a.end(), 0);
                ASSERT_EQ(b.end(), 0);
                EXPECT_EQ(a.context().mass_balance.routing_error(),
                          b.context().mass_balance.routing_error());
            }
}

TEST(EngineCheckpoint, StructuralStateCorruptionAndMaterialChangePreserveFreshTargets) {
    SWMMEngine a;
    ASSERT_NO_THROW(startStructures(a, "structural-atomic-source"));
    SWMMEngine initial;
    ASSERT_NO_THROW(startStructures(initial, "structural-initial"));
    auto opening = root() / "structural-initial.chk";
    ASSERT_EQ(a.saveCheckpoint(opening.string()), "");
    ASSERT_EQ(initial.loadCheckpoint(opening.string()), "");
    compare(a, initial, "structural-opening");
    for (int i = 0; i < 71; ++i)
        ASSERT_NO_THROW(step(a));
    auto good = root() / "structural-atomic.chk";
    ASSERT_EQ(a.saveCheckpoint(good.string()), "");
    detail::CompletedSourceArchive in(bytes(good));
    std::string magic, model, payload;
    uint32_t version, sum;
    in(magic, version, model, sum, payload);
    in.finish();
    ASSERT_EQ(version, 103);
    auto hydraulic = payload.find("OpenSWMM accepted DW hydraulic boundary");
    ASSERT_NE(hydraulic, std::string::npos);
    // Two fixed PID rows (3 doubles each), one orifice and one weir area precede
    // the length-prefixed nested archive. This fixture keeps those shapes explicit.
    const auto begin =
        hydraulic - 2 * sizeof(uint64_t) - (3 * sizeof(uint64_t) + 8 * sizeof(double));
    uint64_t n;
    std::memcpy(&n, payload.data() + begin, sizeof(n));
    ASSERT_EQ(n, 2);
    double e1;
    std::memcpy(&e1, payload.data() + begin + sizeof(uint64_t) + sizeof(double), sizeof(e1));
    EXPECT_NE(e1, 0);
    for (int k = 0; k < 5; ++k) {
        SWMMEngine b;
        auto name = "structural-mutant-" + std::to_string(k);
        SCOPED_TRACE(name);
        ASSERT_NO_THROW(startStructures(b, name));
        auto before = root() / (name + "-before.chk"), after = root() / (name + "-after.chk"),
             bad = root() / (name + "-bad.chk");
        ASSERT_EQ(b.saveCheckpoint(before.string()), "");
        auto data = payload;
        auto v = version;
        if (k == 0) {
            double nan = std::numeric_limits<double>::quiet_NaN();
            std::memcpy(data.data() + begin + sizeof(uint64_t), &nan, sizeof(nan));
        }
        if (k == 1) {
            uint64_t wrong = 3;
            std::memcpy(data.data() + begin, &wrong, sizeof(wrong));
        }
        if (k == 2 || k == 3) {
            double negative = -1;
            auto area = begin + 2 * sizeof(uint64_t) + 6 * sizeof(double) +
                        (k == 3 ? sizeof(uint64_t) + sizeof(double) : 0);
            std::memcpy(data.data() + area, &negative, sizeof(negative));
        }
        if (k == 4)
            v = version - 1;
        sum = crc(data);
        detail::CompletedSourceArchive out;
        out(magic, v, model, sum, data);
        write(bad, out.bytes());
        EXPECT_FALSE(b.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(b.saveCheckpoint(after.string()), "");
        EXPECT_EQ(bytes(before), bytes(after));
        ASSERT_EQ(b.loadCheckpoint(good.string()), "");
        compare(a, b, name + "-retry");
    }
    SWMMEngine c;
    ASSERT_NO_THROW(startStructures(c, "structural-material-change", 0, 0, 0, true, .7));
    EXPECT_FALSE(c.loadCheckpoint(good.string()).empty());
    c.context().link_subtypes.orifices.orate[0] += 1;
    EXPECT_FALSE(c.saveCheckpoint((root() / "structural-changed.chk").string()).empty());
    EXPECT_NE(c.step(nullptr), 0);
}

TEST(EngineCheckpoint, RunoffRainfallAndLegacyLIDContinuationPreservePendingWater) {
    for (int method = 0; method < 5; ++method)
        for (int rain = 0; rain < 3; ++rain)
            for (int node = 0; node < 3; ++node) {
                auto name = "runoff-" + std::to_string(method) + "-" + std::to_string(rain) + "-" +
                            std::to_string(node);
                SCOPED_TRACE(name);
                SWMMEngine a, b;
                ASSERT_NO_THROW(startRunoff(a, name + "-a", method, rain, node));
                ASSERT_NO_THROW(startRunoff(b, name + "-b", method, rain, node));
                // Initial rain state has a gap before its first record.
                auto initial = root() / (name + "-initial.chk");
                ASSERT_EQ(a.saveCheckpoint(initial.string()), "");
                ASSERT_EQ(b.loadCheckpoint(initial.string()), "");
                compare(a, b, name + "-initial");
                while (a.context().current_time < 47)
                    ASSERT_NO_THROW(step(a));
                EXPECT_GT(a.context().subcatches.outfall_runon_vol[2], 0);
                EXPECT_GT(a.context().mass_balance.runoff_rainfall, 0);
                EXPECT_EQ(a.context().gages.co_gage_index[1], 0);
                EXPECT_GT(a.context().gages.past_rain_accum[0], 0);
                auto saved = root() / (name + ".chk");
                ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
                ASSERT_EQ(b.loadCheckpoint(saved.string()), "");
                compare(a, b, name);
                bool dry = false, drain = false, return_to_perv = false, runon = false;
                int count = 0;
                while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
                    ASSERT_NO_THROW(step(a));
                    ASSERT_NO_THROW(step(b));
                    ASSERT_EQ(a.context().current_time, b.context().current_time);
                    dry |= a.context().subcatches.rainfall[0] == 0;
                    for (double v : a.context().subcatches.lid_drain_flow)
                        drain |= v > 0;
                    for (double v : a.context().subcatches.lid_return_to_perv_cfs)
                        return_to_perv |= v > 0;
                    for (double v : a.context().subcatches.runon_inflow)
                        runon |= v > 0;
                    if (++count % 37 == 0)
                        compare(a, b, name);
                }
                compare(a, b, name);
                EXPECT_TRUE(dry);
                EXPECT_TRUE(drain);
                EXPECT_TRUE(return_to_perv);
                EXPECT_TRUE(runon);
                ASSERT_EQ(a.end(), 0);
                ASSERT_EQ(b.end(), 0);
                EXPECT_EQ(a.context().mass_balance.runoff_error(),
                          b.context().mass_balance.runoff_error());
                EXPECT_EQ(a.context().lid_usage.wb_inflow, b.context().lid_usage.wb_inflow);
                EXPECT_EQ(a.context().lid_usage.wb_final_vol, b.context().lid_usage.wb_final_vol);
            }
}

TEST(EngineCheckpoint, RainRulesRetainCompletedHoursAndPartialHourAcrossRestart) {
    const std::string deck =
        "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
        "START_DATE 01/01/2026\nEND_DATE 01/01/2026\nEND_TIME 03:05:00\n"
        "ROUTING_STEP 300\nVARIABLE_STEP 0\nWET_STEP 00:05:00\nDRY_STEP 00:05:00\n"
        "IGNORE_QUALITY YES\nIGNORE_2D YES\nIGNORE_GROUNDWATER YES\nIGNORE_SNOWMELT YES\n"
        "[RAINGAGES]\nRG INTENSITY 1:00 1 TIMESERIES Rain\n"
        "[SUBCATCHMENTS]\nS RG Out 1 100 100 1 0\n[SUBAREAS]\nS .015 .1 0 0 100 OUTLET\n"
        "[INFILTRATION]\nS 3 .5 4 7 0\n[STORAGE]\nTank 10 10 0 FUNCTIONAL 0 0 1000\n"
        "[OUTFALLS]\nOut 0 FREE NO\n[ORIFICES]\nValve Tank Out SIDE 0 .65 NO 0\n"
        "[XSECTIONS]\nValve CIRCULAR 1 0 0 0\n"
        "[TIMESERIES]\nRain 01/01/2026 00:00 1\nRain 01/01/2026 01:00 1\n"
        "Rain 01/01/2026 02:00 1\nRain 01/01/2026 03:00 1\n"
        "[CONTROLS]\nVARIABLE Past48 = GAGE RG 48-HR_DEPTH\n"
        "RULE Wet\nIF Past48 > GAGE RG 1-HR_DEPTH\nTHEN ORIFICE Valve SETTING = .5\n"
        "ELSE ORIFICE Valve SETTING = 0\n[REPORT]\nINPUT NO\nNODES NONE\nLINKS NONE\n";
    auto input = root() / "rain-rule-hours.inp";
    write(input, deck);
    auto initialize = [&](SWMMEngine& e) {
        ASSERT_EQ(e.open(input.string().c_str(), nullptr, nullptr), 0);
        ASSERT_EQ(e.initialize(), 0);
        ASSERT_EQ(e.start(0), 0);
        ASSERT_EQ(e.enableCheckpointing(), "");
    };
    SWMMEngine a, b;
    initialize(a);
    initialize(b);
    while (a.context().current_time < 5400)
        ASSERT_NO_THROW(step(a));
    EXPECT_NEAR(a.context().gages.past_rain[0], 1, 1e-12);
    EXPECT_GT(a.context().gages.past_rain_interval[0], 0);
    auto checkpoint = root() / "rain-rule-hours.chk";
    ASSERT_EQ(a.saveCheckpoint(checkpoint.string()), "");
    ASSERT_EQ(b.loadCheckpoint(checkpoint.string()), "");
    while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(a));
        ASSERT_NO_THROW(step(b));
        EXPECT_EQ(a.context().gages.past_rain, b.context().gages.past_rain);
        EXPECT_EQ(a.context().gages.past_rain_accum, b.context().gages.past_rain_accum);
        EXPECT_EQ(a.context().gages.past_rain_interval, b.context().gages.past_rain_interval);
        EXPECT_EQ(a.context().links.setting, b.context().links.setting);
    }
    EXPECT_DOUBLE_EQ(a.context().links.setting[0], .5);
    // The former record stored absolute calendar seconds; it cannot be
    // interpreted as the new partial-hour interval. Reject it atomically.
    detail::CompletedSourceArchive archive(bytes(checkpoint));
    std::string magic, model, payload;
    uint32_t version, checksum;
    archive(magic, version, model, checksum, payload);
    archive.finish();
    auto marker = payload.find("engine runoff/rainfall boundary v4");
    ASSERT_NE(marker, std::string::npos);
    payload[marker + std::string("engine runoff/rainfall boundary v").size()] = '3';
    checksum = crc(payload);
    detail::CompletedSourceArchive old;
    old(magic, version, model, checksum, payload);
    auto incompatible = root() / "rain-rule-old-record.chk";
    write(incompatible, old.bytes());
    SWMMEngine fresh;
    initialize(fresh);
    EXPECT_FALSE(fresh.loadCheckpoint(incompatible.string()).empty());
    EXPECT_EQ(fresh.context().current_time, 0);
    EXPECT_DOUBLE_EQ(fresh.context().gages.past_rain_interval[0], 0);
    SWMMEngine repeat;
    for (int run = 0; run < 2; ++run) {
        ASSERT_EQ(repeat.open(input.string().c_str(), nullptr, nullptr), 0);
        EXPECT_EQ(repeat.context().control_rules.count(), 2); // VARIABLE block plus RULE
        EXPECT_EQ(repeat.context().link_subtypes.orifices.count(), 1);
        ASSERT_EQ(repeat.initialize(), 0);
        ASSERT_EQ(repeat.start(0), 0);
        while (repeat.context().current_time < 5400)
            ASSERT_NO_THROW(step(repeat));
        EXPECT_NEAR(repeat.context().gages.past_rain[0], 1, 1e-12);
        EXPECT_DOUBLE_EQ(repeat.context().gages.past_rain[1], 0);
        ASSERT_EQ(repeat.end(), 0);
        ASSERT_EQ(repeat.close(), 0);
    }
}

TEST(EngineCheckpoint, RunoffCorruptionAndUnsupportedProfilesPreserveFreshTargets) {
    SWMMEngine a;
    ASSERT_NO_THROW(startRunoff(a, "runoff-atomic-source", 2, 2, 2));
    while (a.context().current_time < 47)
        ASSERT_NO_THROW(step(a));
    auto good = root() / "runoff-atomic.chk";
    ASSERT_EQ(a.saveCheckpoint(good.string()), "");
    detail::CompletedSourceArchive in(bytes(good));
    std::string magic, model, payload;
    uint32_t version, sum;
    in(magic, version, model, sum, payload);
    in.finish();
    const std::string tag = "engine runoff/rainfall boundary v4";
    auto begin = payload.find(tag);
    ASSERT_NE(begin, std::string::npos);
    begin += tag.size();
    detail::CompletedSourceArchive water_in(payload.substr(begin));
    std::string water, units;
    water_in(water, units);
    const auto runoff_at = begin + sizeof(uint64_t),
               lid_at = runoff_at + water.size() + sizeof(uint64_t);
    const auto sub_at = lid_at + units.size();
    // Walk only the explicit nested layout; mutations retain a valid CRC.
    auto skip = [](const std::string& data, std::size_t& offset,
                   std::size_t width = sizeof(double)) {
        uint64_t n;
        std::memcpy(&n, data.data() + offset, sizeof(n));
        offset += sizeof(n) + n * width;
    };
    std::size_t bank = 0;
    for (int i = 0; i < 23; ++i)
        skip(water, bank);
    uint64_t n;
    std::memcpy(&n, water.data() + bank, sizeof(n));
    ASSERT_EQ(n, 3);
    bank += sizeof(n) + n * 3 * 10 * sizeof(double);
    skip(water, bank);
    skip(water, bank, 8 * sizeof(double)); // cumulative, Horton rows
    const auto ga = bank + sizeof(uint64_t);
    for (int k = 0; k < 9; ++k) {
        SWMMEngine b;
        auto name = "runoff-mutant-" + std::to_string(k);
        SCOPED_TRACE(name);
        ASSERT_NO_THROW(startRunoff(b, name, 2, 2, 2));
        auto before = root() / (name + "-before.chk"), after = root() / (name + "-after.chk"),
             bad = root() / (name + "-bad.chk");
        ASSERT_EQ(b.saveCheckpoint(before.string()), "");
        auto data = payload;
        auto put = [&](std::size_t at, double v) { std::memcpy(data.data() + at, &v, sizeof(v)); };
        if (k == 0)
            put(runoff_at + sizeof(uint64_t), -1);
        if (k == 1)
            put(runoff_at + sizeof(uint64_t), std::numeric_limits<double>::quiet_NaN());
        if (k == 2) {
            uint64_t wrong = 4;
            std::memcpy(data.data() + runoff_at, &wrong, sizeof(wrong));
        }
        if (k == 3)
            put(runoff_at + ga, 123); // GA material
        if (k == 4)
            put(runoff_at + ga + 3 * sizeof(double), 2); // GA moisture deficit
        if (k == 5) {
            std::size_t at = sizeof(uint64_t);
            for (int i = 0; i < 6; ++i)
                skip(units, at);
            skip(units, at, sizeof(int));
            skip(units, at);
            skip(units, at);
            put(lid_at + at + sizeof(uint64_t), -1);
        }
        if (k == 6)
            put(sub_at + sizeof(uint64_t), -1);
        if (k == 7) {
            detail::CompletedSourceArchive cursors;
            auto cur = a.context().gages.st_cur, next = a.context().gages.st_next;
            cursors(cur, next);
            auto at = data.find(cursors.bytes(), sub_at);
            ASSERT_NE(at, std::string::npos);
            int wrong = 999;
            std::memcpy(data.data() + at + sizeof(uint64_t), &wrong, sizeof(wrong));
        }
        if (k == 8) {
            double stored = a.context().mass_balance.runoff_final_store;
            detail::CompletedSourceArchive value;
            value(stored);
            auto at = data.find(value.bytes());
            ASSERT_NE(at, std::string::npos);
            put(at, stored + 1);
        }
        sum = crc(data);
        detail::CompletedSourceArchive out;
        out(magic, version, model, sum, data);
        write(bad, out.bytes());
        EXPECT_FALSE(b.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(b.saveCheckpoint(after.string()), "");
        EXPECT_EQ(bytes(before), bytes(after));
        ASSERT_EQ(b.loadCheckpoint(good.string()), "");
        compare(a, b, name + "-retry");
    }
    for (int k = 0; k < 6; ++k) {
        SWMMEngine b;
        ASSERT_NO_THROW(startRunoff(b, "runoff-profile-" + std::to_string(k), 0, 0, 0, false));
        if (k == 0)
            b.context().options.ignore_snow_melt = false;
        if (k == 1)
            b.context().options.ignore_groundwater = false;
        if (k == 2)
            b.context().gages.source[0] = RainSource::FILE_RAIN;
        if (k == 3)
            b.context().gages.api_rainfall[0] = 1;
        if (k == 4)
            b.context().lid_usage.rpt_file[0] = "detailed.txt";
        if (k == 5)
            b.context().subcatches.gw_aquifer[0] = 0;
        if (k == 0)
            EXPECT_EQ(b.enableCheckpointing(),
                      ""); // native snow is part of the accepted checkpoint
        else
            EXPECT_FALSE(b.enableCheckpointing().empty());
    }
    SWMMEngine changed;
    ASSERT_NO_THROW(startRunoff(changed, "runoff-model-change", 2, 2, 2));
    changed.context().subcatches.infil_p1[0] += 1;
    EXPECT_FALSE(changed.loadCheckpoint(good.string()).empty());
    EXPECT_FALSE(changed.saveCheckpoint((root() / "runoff-changed.chk").string()).empty());
}

TEST(EngineCheckpoint, RunoffGapDryRecessionAndIgnoredRainRestore) {
    for (int method = 0; method < 5; ++method)
        for (int phase = 0; phase < 3; ++phase) {
            auto name = "runoff-phase-" + std::to_string(method) + "-" + std::to_string(phase);
            SCOPED_TRACE(name);
            SWMMEngine a, b;
            ASSERT_NO_THROW(startRunoff(a, name + "-a", method, 2, 2, false));
            ASSERT_NO_THROW(startRunoff(b, name + "-b", method, 2, 2, false));
            if (phase == 2) {
                a.context().options.ignore_rainfall = true;
                b.context().options.ignore_rainfall = true;
            }
            ASSERT_EQ(a.enableCheckpointing(), "");
            ASSERT_EQ(b.enableCheckpointing(), "");
            while (a.context().current_time < (phase == 0 ? 5 : 150))
                ASSERT_NO_THROW(step(a));
            EXPECT_EQ(a.context().subcatches.rainfall[0], 0);
            auto p = root() / (name + ".chk");
            ASSERT_EQ(a.saveCheckpoint(p.string()), "");
            ASSERT_EQ(b.loadCheckpoint(p.string()), "");
            while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
                ASSERT_NO_THROW(step(a));
                ASSERT_NO_THROW(step(b));
            }
            compare(a, b, name);
            if (phase == 2)
                EXPECT_EQ(a.context().mass_balance.runoff_rainfall, 0);
        }
}

#ifdef OPENSWMM_HAS_2D
namespace {
struct SurfaceSession {
    // The qualification source objects outlive the engine's non-owning attachment.
    runoff::SourceWaterDriver sources;
    twoD::CommonSourceStage stage;
    SWMMEngine engine;
    /**
     * @brief Build and start an owned surface/GW/source checkpoint profile.
     *
     * Owns source driver, common stage and engine; declaration order keeps non-owning source
     * attachments alive until engine destruction. Builds two SI triangles over the CFS network with
     * explicit unit conversion and near-saturation GW profiles, prepares selected output checkpoints
     * before start, then installs reviewed source ownership and checkpoint attachment. Intended for a
     * fresh session; profile selectors and raw names are not validated beyond downstream APIs. Throws
     * runtime_error on rejected lifecycle/config/stage/checkpoint operations and leaves partial member
     * state for normal ownership cleanup; unchecked file writes/selection combinations can also fail
     * downstream.
     *
     * @param[in] name Raw evidence stem below root; matching input/report and selected result files may
     *     be overwritten or removed.
     * @param[in] lid NODE LID mode forwarded to the authored runoff network.
     * @param[in] mapped Triangle-to-node profile: 0 maps both triangles to node zero, 1 reversed {2,0}, 2 common
     *     node two, 3 outfall/node-zero mapping.
     * @param[in] closure 0 CLOSED_FORM, 1 ENSLAVED, 2 SIGMA groundwater closure.
     * @param[in] mode Nonnegative profile selector; modulo 5/3 chooses infiltration/rain format and
     *     modulo 4 surcharge method.
     * @param[in] track Enable spatial or ordinary attached checkpointing after setup when true.
     * @param[in] spatial Build reviewed half-m2 source footprints and use spatial-source checkpointing
     *     when true.
     * @param[in] all_sources Include source rows zero/two for spatial selection and disable the
     *     original outfall route before any explicit return override.
     * @param[in] outfall_return Nonnegative destination assigned to every outfall return and enables
     *     return tracking; negative keeps fixture default routing.
     * @param[in] output Open a binary-result destination and request saved results when true.
     * @param[in] mesh_output 0 none; nonzero adds HDF5 mesh output, 2 FLOAT32/compression4, 3 minimal
     *     variables, 4 delayed report start; other nonzero uses FLOAT64.
     * @param[in] average Enable averaged 1D reporting when true.
     * @param[in] runtime_identity Borrowed runtime-forcing identity copied by stage initialization.
     * @param[in] runtime_boundaries Authorize runtime water-boundary support in the stage.
     * @param[in] authored_boundary 0 none, 1..6 one boundary law, 7 mixed laws, 8 equivalent dated
     *     time-series signals; must be nonnegative.
     * @param[in] signal_variant Exact value one offsets authored stage signals by 0.1 m; other values
     *     keep their baseline.
     * @param[in] catchment_forcing Enable catchment runtime forcing support in stage initialization.
     * @param[in] routing_cadence Positive override in seconds fixes routing cadence and disables
     *     variable step; nonpositive retains authored timing.
     * @param[in] thermal Enable coupled surface/ground-column heat configuration when true.
     */
    void start(const std::string& name, int lid = 0, int mapped = 0, int closure = 0, int mode = 0,
               bool track = true, bool spatial = false, bool all_sources = false,
               int outfall_return = -1, bool output = false, int mesh_output = 0,
               bool average = false, const std::string& runtime_identity = {},
               bool runtime_boundaries = false, int authored_boundary = 0, int signal_variant = 0,
               bool catchment_forcing = false, double routing_cadence = 0, bool thermal = false) {
        SWMMEngine authored;
        startRunoff(authored, name, mode % 5, mode % 3, lid, false);
        authored.close();
        auto p = root() / name;
        auto deck = bytes(p.string() + ".inp");
        deck.replace(deck.find("IGNORE_2D YES"), 13, "IGNORE_2D NO");
        // Surface advances on every routing step; routing-event skips are outside this profile.
        if (mesh_output) {
            std::filesystem::remove(p.string() + ".h5");
            deck += "[2D_OPTIONS]\nOUTPUT_FILE " + p.string() + ".h5\nOUTPUT_PRECISION " +
                    (mesh_output == 2 ? std::string("FLOAT32") : std::string("FLOAT64")) +
                    "\nOUTPUT_COMPRESSION " +
                    (mesh_output == 2 ? std::string("4") : std::string("0")) +
                    "\nREPORT_2D_STEP 00:00:06\n";
            if (mesh_output == 3)
                deck += "REPORT_2D_VARIABLES MINIMAL\n";
            if (mesh_output == 4)
                deck += "[OPTIONS]\nREPORT_START_DATE 01/31/2004\nREPORT_START_TIME 23:59:13\n";
        }
        if (authored_boundary) {
            const char* rows[] = {
                "",
                "0 1 NORMAL_FLOW .03\n",
                "0 1 SPECIFIED_STAGE .52\n",
                "0 1 SPECIFIED_FLOW .0005\n",
                "0 1 RATING_CURVE EdgeRating\n",
                "0 1 TS_STAGE EdgeStage\n",
                "0 1 TS_FLOW EdgeFlow\n",
                "0 1 TS_STAGE EdgeStage\n0 2 TS_FLOW EdgeFlow\n1 0 RATING_CURVE EdgeRating\n1 2 NORMAL_FLOW .03\n"};
            deck +=
                "[2D_BOUNDARY_CONDITIONS]\n" + std::string(rows[std::min(authored_boundary, 7)]);
            deck += "[TIMESERIES]\n";
            // Relative records are anchored to the model start; dated records
            // represent the same signal in the model's absolute calendar.
            for (const auto& row : std::vector<std::pair<const char*, double>>{
                     {"00", 1.7}, {"04", 1.95}, {"08", 1.65}, {"20", 1.8}})
                deck += "EdgeStage " +
                        std::string(authored_boundary == 8 ? "01/31/2004 23:59:" : "00:00:") +
                        row.first + " " +
                        std::to_string(row.second + (signal_variant == 1 ? .1 : 0)) + "\n";
            deck +=
                "EdgeFlow 00:00:00 -.01\nEdgeFlow 00:00:04 .015\nEdgeFlow 00:00:08 -.008\nEdgeFlow 00:00:20 .012\n";
            deck +=
                "[CURVES]\nEdgeRating RATING 0 0\nEdgeRating 1.6 .005\nEdgeRating 2.5 .025\nEdgeRating 20 .03\n";
        }
        if (thermal)
            deck += "\n[OPTIONS]\nHEAT_TRANSPORT YES\n";
        write(p.string() + ".inp", deck);
        auto& e = engine;
        if (output)
            std::filesystem::remove(p.string() + ".out");
        if (e.open((p.string() + ".inp").c_str(), (p.string() + ".rpt").c_str(),
                   output ? (p.string() + ".out").c_str() : nullptr))
            throw std::runtime_error(e.context().error_message);
        auto& ctx = e.context();
        ctx.options.num_threads = 1;
        ctx.options.surcharge_method = mode % 4;
        if (thermal) {
            if (swmm_options_set_ext(&e, "HEAT_MODEL", "COUPLED") != SWMM_OK ||
                swmm_options_set_ext(&e, "BED_HEAT_MODEL", "GROUND_COLUMN") != SWMM_OK ||
                swmm_options_set_ext(&e, "HEAT_SURFACE_EXCHANGE", "NO") != SWMM_OK ||
                swmm_options_set_ext(&e, "HEAT_RADIATIVE_EXCHANGE", "NO") != SWMM_OK ||
                swmm_gw_transport_option_set(&e, "THERMAL_MODEL", "POROUS_EQUILIBRIUM") !=
                    SWMM_OK ||
                swmm_gw_transport_option_set(&e, "CONDUCTION", "YES") != SWMM_OK ||
                swmm_gw_transport_option_set(&e, "SURFACE_THERMAL_BC", "ADIABATIC") != SWMM_OK ||
                swmm_gw_transport_option_set(&e, "DEEP_THERMAL_BC", "ADIABATIC") != SWMM_OK)
                throw std::runtime_error("thermal checkpoint fixture configuration failed");
        }
        if (routing_cadence > 0) {
            ctx.options.routing_step = routing_cadence;
            ctx.options.variable_step = 0;
        }
        ctx.events.clear();
        ctx.gages.scale_factor[1] = 1.3;
        ctx.node_subtypes.outfalls.route_to[0] = 2;
        auto& router = e.surfaceRouter2D();
        auto& mesh = router.mesh();
        mesh.resize_vertices(4);
        mesh.resize_triangles(2);
        mesh.vx = {0, 4, 0, 4};
        mesh.vy = {0, 0, 2, 2};
        mesh.vz = {.5, .5, .5, .5};
        mesh.set_triangle(0, 0, 1, 2);
        mesh.set_triangle(1, 1, 3, 2);
        mesh.mannings_n = {.03, .03};
        mesh.tri_init_depth = {.04, .003};
        mesh.tri_coupled_node =
            mapped == 1
                ? std::vector<int>{2, 0}
                : (mapped == 2 ? std::vector<int>{2, 2}
                               : (mapped == 3 ? std::vector<int>{ctx.node_names.find("O"), 0}
                                              : std::vector<int>{0, 0}));
        mesh.tri_coupling_cd = {.6, .6};
        mesh.tri_coupling_area = {.002, .003};
        auto& options = router.options();
        options.mesh_units_si = true;
        options.num_threads = 1;
        options.momentum = twoD::Momentum2D::LOCAL_INERTIAL;
        options.max_timestep = .21;
        options.coupling_sync = 3.5;
        options.evaporation = 2;
        options.rainfall_mode = twoD::RainfallMode::SYSTEM;
        auto& config = router.aquiferConfig();
        config.options.authored = true;
        config.options.force_closed_form = true;
        config.options.gw_et = "BOUNDARY_ET";
        config.options.closure = closure == 2 ? twoD::GwClosure::SIGMA
                                 : closure    ? twoD::GwClosure::ENSLAVED
                                              : twoD::GwClosure::CLOSED_FORM;
        twoD::GwAquiferRow row;
        row.Ks = .000001 * 3600 / .0254;
        row.zs = 2 / .3048;
        row.hg0 = 1 / .3048;
        row.alpha = 2 * .3048;
        config.rows.push_back(row);
        ctx.options.rpt_averages = average;
        if (e.initialize())
            throw std::runtime_error("surface initialize: " + ctx.error_message);
        if (output && mesh_output) {
            auto error = e.prepareCombinedOutputCheckpointing();
            if (!error.empty())
                throw std::runtime_error(error);
        } else if (output) {
            auto error = e.prepareOutputCheckpointing();
            if (!error.empty())
                throw std::runtime_error(error);
        } else if (mesh_output) {
            auto error = e.prepareSurfaceOutputCheckpointing();
            if (!error.empty())
                throw std::runtime_error(error);
        }
        if (e.start(output ? 1 : 0))
            throw std::runtime_error("surface start: " + ctx.error_message);
        auto& gw = router.subsurface().state();
        for (int i = 0; i < 2 && closure < 2; ++i) {
            if (closure)
                gw.hg[i] = gw.zs[i] - .0001;
            gw.hu[i] = gw.theta_s[i] * (gw.zs[i] - gw.hg[i]) - .0001 / mesh.tri_area[i];
        }
        if (all_sources)
            ctx.node_subtypes.outfalls.route_to[0] = -1;
        if (outfall_return >= 0)
            std::fill(ctx.node_subtypes.outfalls.route_to.begin(),
                      ctx.node_subtypes.outfalls.route_to.end(), outfall_return);
        auto error = sources.initialize(
            ctx,
            spatial ? (all_sources ? std::vector<int>{0, 2} : std::vector<int>{0})
                    : std::vector<int>{},
            {}, nullptr, outfall_return >= 0);
        if (!error.empty())
            throw std::runtime_error(error);
        twoD::SurfaceOwnershipPreview preview;
        preview.mesh_weather_area = mesh.tri_area;
        if (spatial) {
            constexpr double ft2 = .3048 * .3048;
            for (const auto& group : sources.clocks().groups())
                for (int sc : group.subcatches) {
                    const auto& r = sources.runoff().soa();
                    const double lid_area =
                        sources.context().subcatches.total_lid_area_ft2[sc] * ft2;
                    const double full = r.area[sc] * ft2 + lid_area;
                    const double inside = .5;
                    const int cell = sc % 2;
                    twoD::SurfaceOwnerObject object;
                    object.subcatch = sc;
                    object.reviewed = true;
                    object.declared_area = full;
                    object.inside_area = inside;
                    object.outside_area = full - inside;
                    preview.objects.push_back(object);
                    twoD::SurfaceOwnerShare share;
                    share.subcatch = sc;
                    share.cell = cell;
                    share.weather_area = inside;
                    share.pervious_area = inside * r.area[sc] * (1 - r.imperv_pct[sc]) * ft2 / full;
                    share.impervious_area = inside * r.area[sc] * r.imperv_pct[sc] * ft2 / full;
                    share.lid_area = inside * lid_area / full;
                    preview.shares.push_back(share);
                    preview.mesh_weather_area[cell] -= inside;
                }
        }
        error =
            stage.initialize(mesh, router.state(), options, router.subsurface(), sources, preview,
                             !spatial, runtime_identity, runtime_boundaries, catchment_forcing);
        if (!error.empty())
            throw std::runtime_error(error);
        if (track) {
            error =
                spatial ? e.enableSpatialSourceCheckpointing(stage) : e.enableCheckpointing(stage);
            if (!error.empty())
                throw std::runtime_error(error);
        }
    }
};
struct ThermalSurfaceSession {
    runoff::SourceWaterDriver sources;
    twoD::CommonSourceStage stage;
    SWMMEngine engine;
    std::string native_hotstart_save, native_hotstart_use;
    std::string native_hotstart_apply;
    std::string extra_files; // more [FILES] rows
    std::string extra_input;
    /**
     * @brief Build and start the owned thermal surface/bed/groundwater continuation fixture.
     *
     * Builds two SI cells in a CMS three-minute deck with one-second routing. Members
     * extra_input/extra_files/native_hotstart_* add raw config or save/use/apply behavior; strings are
     * copied without escaping/combination validation. Initializes, optionally applies a native
     * hotstart, prepares output then starts without binary saved results. If start already attached a
     * session, retains it; otherwise initializes owned source/stage members and optionally enables
     * checkpointing. Source members outlive engine non-owning attachments by declaration order.
     * Lifecycle/option/hotstart/checkpoint failures throw runtime_error with partial state retained;
     * caller must use physically compatible flag combinations on a fresh object.
     *
     * @param[in] name Raw artifact stem below root; matching deck/report and requested HDF5 output may
     *     be overwritten or removed.
     * @param[in] surface_temperature Initial surface heat-source temperature in degrees C.
     * @param[in] channel_tag Tag assigned to mesh cell zero; cell one receives floodplain.
     * @param[in] dry_surface Start both mesh cells dry instead of 0.04/0.03 m depth.
     * @param[in] atmospheric_exchange Enable HEAT_SURFACE_EXCHANGE when true; radiation remains
     *     disabled here.
     * @param[in] temperature_timeseries Author 5-to-35 C AirTemp records plus humidity/monthly wind
     *     when true.
     * @param[in] lateral_conduction Enable groundwater lateral conduction through the option API when
     *     true.
     * @param[in] heterogeneous_ground Override both groundwater zones to 10 C in cell zero and 20 C in
     *     cell one.
     * @param[in] mapped_channel Add a live 1D channel and ground-source/sediment-exchange coupling
     *     settings.
     * @param[in] enable_checkpoint Attach checkpointing after start when no managed/loaded session was
     *     already attached.
     * @param[in] ueb_snow Install a UEB mesh snowpack/site/monthly-range profile when true.
     * @param[in] mesh_richards_lid Add a Richards NODE LID with MESH_AQUIFER bottom contact on cell
     *     zero.
     * @param[in] mesh_output Prepare FLOAT64 depth/heat HDF5 output checkpointing before start when
     *     true.
     * @param[in] local_storage Use LOCAL_STORAGE thermal bed instead of ground-column aquifer profile.
     * @param[in] local_storage_quality Imply local_storage and add Dye/age bed transport with 0.001 m/s
     *     hyporheic exchange.
     * @param[in] ground_column_quality Add groundwater Dye/age rows and 0.001 m/s hyporheic exchange.
     * @param[in] closure Raw aquifer closure keyword; exact CLOSED_FORM enables force_closed_form.
     * @param[in] lid_layer_conduction Enable LID layer conduction after initialization and before
     *     start.
     */
    void start(const std::string& name, double surface_temperature = 12,
               const std::string& channel_tag = "channel:test", bool dry_surface = false,
               bool atmospheric_exchange = false, bool temperature_timeseries = false,
               bool lateral_conduction = false, bool heterogeneous_ground = false,
               bool mapped_channel = false, bool enable_checkpoint = true, bool ueb_snow = false,
               bool mesh_richards_lid = false, bool mesh_output = false, bool local_storage = false,
               bool local_storage_quality = false, bool ground_column_quality = false,
               const std::string& closure = "CLOSED_FORM", bool lid_layer_conduction = false) {
        local_storage = local_storage || local_storage_quality;
        const auto p = root() / name;
        std::string deck =
            "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT YES\n"
            "START_DATE 01/01/2026\nSTART_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 00:03:00\n"
            "ROUTING_STEP 1\nREPORT_STEP 00:00:10\nIGNORE_2D NO\nIGNORE_QUALITY YES\n"
            "[2D_OPTIONS]\nREPORT_2D NO\nMAX_TIMESTEP .2\nLTS_TIERS 1\nEVAPORATION NO\nINFILTRATION NO\n";
        if (!native_hotstart_save.empty() || !native_hotstart_use.empty() || !extra_files.empty()) {
            deck += "\n[FILES]\n";
            if (!native_hotstart_use.empty())
                deck += "USE HOTSTART \"" + native_hotstart_use + "\"\n";
            if (!native_hotstart_save.empty())
                deck += "SAVE HOTSTART \"" + native_hotstart_save + "\"\n";
            deck += extra_files;
        }
        if (!local_storage)
            deck +=
                "[2D_AQUIFER_OPTIONS]\nNODE_ENROLMENT ROWS\nGW_ET NONE\nM_LAYERS 4\nCLOSURE " +
                closure +
                "\n"
                "[GW_TRANSPORT_OPTIONS]\nTHERMAL_MODEL POROUS_EQUILIBRIUM\nCONDUCTION YES\nDISPERSION NO\n"
                "SOIL_ET_HEAT SENSIBLE_ONLY\nSURFACE_THERMAL_BC ADIABATIC\nDEEP_THERMAL_BC ADIABATIC\n"
                "[GW_INITIAL_QUALITY]\n* SAT __TEMPERATURE__ 8\n* UNSAT __TEMPERATURE__ 8\n";
        if (mesh_richards_lid)
            deck += "[STORAGE]\nLID 0 .024 .01 FUNCTIONAL 0 0 100 0 0\n"
                    "[COORDINATES]\nLID .2 .5\n"
                    "[2D_AQUIFER_NODE]\nLID 1 KC 3600 DC .1 AREA 1\n"
                    "[LID_CONTROLS]\nStack NODE\nStack SURFACE 6 .1\n"
                    "Stack MEDIA 12 .45 .2 .08 2 10 3\nStack AGGREGATE 6 .4 100\n"
                    "Stack BOTTOM 0 0\n[LID_NODES]\nLID Stack 1 BOTTOM MESH_AQUIFER\n"
                    "[LID_RICHARDS]\nStack OPTIONS 4 1e-7 1e-5 30\n"
                    "Stack 2 .03 2 1.6 .5 .0001\nStack 3 .03 2 1.6 .5 .0001\n";
        if (temperature_timeseries)
            deck += "[TEMPERATURE]\nTIMESERIES AirTemp\nHUMIDITY 50\n"
                    "WINDSPEED MONTHLY 5 5 5 5 5 5 5 5 5 5 5 5\n"
                    "[TIMESERIES]\nAirTemp 01/01/2026 00:00:00 5\n"
                    "AirTemp 01/01/2026 00:00:20 35\nAirTemp 01/01/2026 00:03:00 35\n";
        if (mapped_channel) {
            deck.replace(deck.find("IGNORE_QUALITY YES"), std::string("IGNORE_QUALITY YES").size(),
                         "IGNORE_QUALITY NO");
            deck += "[JUNCTIONS]\nJ1 .2 4 0 0 0\n[OUTFALLS]\nO1 .2 FREE NO\n"
                    "[CONDUITS]\nC1 J1 O1 3.6 .013 0 0 0\n"
                    "[XSECTIONS]\nC1 RECT_OPEN .5 .1 0 0 1\n"
                    "[COORDINATES]\nJ1 .2 .5\nO1 3.8 .5\n[DWF]\nJ1 FLOW .01\n";
        }
        if (mesh_richards_lid)
            deck.replace(deck.find("IGNORE_QUALITY YES"), std::string("IGNORE_QUALITY YES").size(),
                         "IGNORE_QUALITY NO");
        if (local_storage_quality) {
            deck.replace(deck.find("IGNORE_QUALITY YES"), std::string("IGNORE_QUALITY YES").size(),
                         "IGNORE_QUALITY NO");
            deck += "\n[OPTIONS]\nWATER_AGE ON\n[POLLUTANTS]\nDye MG/L 0 0 0 0 NO * 0 0\n"
                    "[2D_OPTIONS]\nTRANSPORT_POLLUTANTS YES\nTRANSPORT_AGE YES\n"
                    "[2D_INITIAL_QUALITY]\n* Dye 10\n"
                    "[2D_INITIAL_BED_QUALITY]\n* Dye 2\n* __WATER_AGE__ 1\n";
        }
        if (ground_column_quality) {
            const auto ignored = deck.find("IGNORE_QUALITY YES");
            if (ignored != std::string::npos)
                deck.replace(ignored, std::string("IGNORE_QUALITY YES").size(),
                             "IGNORE_QUALITY NO");
            deck += "\n[OPTIONS]\nWATER_AGE ON\n[POLLUTANTS]\nDye MG/L 0 0 0 0 NO * 0 0\n"
                    "[2D_OPTIONS]\nTRANSPORT_POLLUTANTS YES\nTRANSPORT_AGE YES\n"
                    "[2D_INITIAL_QUALITY]\n* Dye 0\n"
                    "[GW_INITIAL_QUALITY]\n* SAT Dye 10\n* UNSAT Dye 10\n";
        }
        if (mesh_output) {
            std::filesystem::remove(p.string() + ".h5");
            deck += "\n[2D_OPTIONS]\nREPORT_2D YES\nOUTPUT_FILE " + (p.string() + ".h5") +
                    "\nOUTPUT_PRECISION FLOAT64\nREPORT_2D_VARIABLES DEPTH HEAT\n";
        }
        deck += extra_input;
        write(p.string() + ".inp", deck);
        if (engine.open((p.string() + ".inp").c_str(), (p.string() + ".rpt").c_str(), nullptr))
            throw std::runtime_error("thermal surface open: " + engine.context().error_message);
        if (swmm_options_set_ext(&engine, "HEAT_MODEL", "COUPLED") != SWMM_OK ||
            swmm_options_set_ext(&engine, "BED_HEAT_MODEL",
                                 local_storage ? "LOCAL_STORAGE" : "GROUND_COLUMN") != SWMM_OK ||
            swmm_options_set_ext(&engine, "HEAT_SURFACE_EXCHANGE",
                                 atmospheric_exchange ? "YES" : "NO") != SWMM_OK ||
            swmm_options_set_ext(&engine, "HEAT_RADIATIVE_EXCHANGE", "NO") != SWMM_OK)
            throw std::runtime_error("thermal surface configuration failed");
        if (local_storage_quality &&
            swmm_heat_set_sediment(&engine, SWMM_HEAT_SED_HYPORHEIC_VELOCITY, .001) != SWMM_OK)
            throw std::runtime_error("local-bed quality exchange configuration failed");
        if (ground_column_quality &&
            swmm_heat_set_sediment(&engine, SWMM_HEAT_SED_HYPORHEIC_VELOCITY, .001) != SWMM_OK)
            throw std::runtime_error("ground-column quality exchange configuration failed");
        if (!local_storage &&
            (swmm_gw_transport_option_set(&engine, "THERMAL_MODEL", "POROUS_EQUILIBRIUM") !=
                 SWMM_OK ||
             swmm_gw_transport_option_set(&engine, "CONDUCTION", "YES") != SWMM_OK ||
             swmm_gw_transport_option_set(&engine, "SURFACE_THERMAL_BC", "ADIABATIC") != SWMM_OK ||
             swmm_gw_transport_option_set(&engine, "DEEP_THERMAL_BC", "ADIABATIC") != SWMM_OK))
            throw std::runtime_error("ground thermal surface configuration failed");
        if (lateral_conduction &&
            swmm_gw_transport_option_set(&engine, "LATERAL_CONDUCTION", "YES") != SWMM_OK)
            throw std::runtime_error("thermal lateral conduction configuration failed");
        if (mapped_channel &&
            (swmm_heat_set_sediment(&engine, SWMM_HEAT_SED_GROUND_SOURCE, 1) != SWMM_OK ||
             swmm_heat_set_sediment(&engine, SWMM_HEAT_SED_HYPORHEIC_VELOCITY, 2e-5) != SWMM_OK ||
             swmm_heat_set_sediment(&engine, SWMM_HEAT_SED_BED_THICKNESS, .1) != SWMM_OK ||
             swmm_heat_set_module(&engine, SWMM_HEAT_SEDIMENT_EXCHANGE, 1) != SWMM_OK))
            throw std::runtime_error("thermal channel contact configuration failed");
        auto& ctx = engine.context();
        ctx.options.num_threads = 1;
        ctx.heat_config.global_temp[static_cast<int>(HeatSource::INITIAL_STATE)] =
            surface_temperature;
        auto& router = engine.surfaceRouter2D();
        auto& mesh = router.mesh();
        mesh.resize_vertices(4);
        mesh.resize_triangles(2);
        mesh.vx = {0, 4, 0, 4};
        mesh.vy = {0, 0, 2, 2};
        mesh.vz = {.5, .5, .5, .5};
        mesh.set_triangle(0, 0, 1, 2);
        mesh.set_triangle(1, 1, 3, 2);
        mesh.mannings_n = {.03, .03};
        mesh.tri_init_depth =
            dry_surface ? std::vector<double>{0, 0} : std::vector<double>{.04, .03};
        mesh.tri_tag = {channel_tag, "floodplain"};
        if (mesh_richards_lid) {
            mesh.tri_coupled_node = {ctx.node_names.find("LID"), -1};
            mesh.tri_coupling_cd = {0, 0};
            mesh.tri_coupling_area = {1, 1};
        }
        if (ueb_snow) {
            auto& c = engine.context();
            c.options.ignore_snow_melt = false;
            c.snowpack_names.try_add("Snow");
            c.snowpacks.names = {"Snow"};
            c.snowpacks.plowable = c.snowpacks.impervious =
                c.snowpacks.pervious = {std::array<double, 7>{1, 2, 0, .1, 100, 10, 0}};
            c.snowpacks.removal = {std::array<double, 6>{}};
            c.snowpacks.removal_subcatch = {""};
            c.snowpacks.config.model = surface::SnowKind::Ueb;
            c.snowpacks.config.step = 60;
            c.snowpacks.config.monthly_range.fill(10.);
            c.snowpacks.config.site = surface::ClimateSite{45, -120, -8, 1000};
            c.snowpacks.mesh_config.defaults["*"].pack = "Snow";
        }
        if (heterogeneous_ground)
            for (int cell = 0; cell < 2; ++cell)
                for (int zone : {SWMM_GW_ZONE_SAT, SWMM_GW_ZONE_UNSAT})
                    if (swmm_gw_init_quality_set(&engine, SWMM_GW_SCOPE_CELL, nullptr, cell, zone,
                                                 -1, "__TEMPERATURE__",
                                                 cell == 0 ? 10 : 20) != SWMM_OK)
                        throw std::runtime_error(
                            "thermal initial layer temperatures configuration failed");
        auto& options = router.options();
        options.mesh_units_si = true;
        options.num_threads = 1;
        options.momentum = twoD::Momentum2D::LOCAL_INERTIAL;
        options.max_timestep = .2;
        options.coupling_sync = 1;
        options.rainfall_mode = twoD::RainfallMode::SYSTEM;
        if (!local_storage) {
            auto& config = router.aquiferConfig();
            config.options.authored = true;
            config.options.force_closed_form = closure == "CLOSED_FORM";
            twoD::GwAquiferRow row;
            row.Ks = .000001 * 3600 / .0254;
            row.zs = 2 / .3048;
            row.hg0 = 1 / .3048;
            row.alpha = 2 * .3048;
            config.rows.push_back(row);
        }
        if (engine.initialize())
            throw std::runtime_error("thermal surface initialize: " + ctx.error_message);
        if (!native_hotstart_apply.empty()) {
            SWMM_HotStart hotstart = nullptr;
            if (swmm_hotstart_open(native_hotstart_apply.c_str(), &hotstart) != SWMM_OK)
                throw std::runtime_error("thermal native hotstart open failed");
            const int applied = swmm_hotstart_apply(&engine, hotstart);
            swmm_hotstart_close(hotstart);
            if (applied != SWMM_OK)
                throw std::runtime_error("thermal native hotstart queue failed");
        }
        if (lid_layer_conduction &&
            swmm_heat_set_module(&engine, SWMM_HEAT_LAYER_CONDUCTION, 1) != SWMM_OK)
            throw std::runtime_error("mesh LID layer conduction configuration failed");
        if (mesh_output)
            if (auto error = engine.prepareSurfaceOutputCheckpointing(); !error.empty())
                throw std::runtime_error("thermal mesh output checkpoint setup: " + error);
        if (engine.start(0))
            throw std::runtime_error("thermal surface start: " + ctx.error_message);
        if ((engine.checkpointSessionStatus() & SWMM_CHECKPOINT_SESSION_ATTACHED) == 0) {
            if (auto error = sources.initialize(ctx, {}, {}); !error.empty())
                throw std::runtime_error(error);
            twoD::SurfaceOwnershipPreview preview;
            preview.mesh_weather_area = mesh.tri_area;
            if (auto error = stage.initialize(mesh, router.state(), options, router.subsurface(),
                                              sources, preview, true);
                !error.empty())
                throw std::runtime_error(error);
            if (enable_checkpoint)
                if (auto error = engine.enableCheckpointing(stage); !error.empty())
                    throw std::runtime_error(
                        error + " [nodes=" + std::to_string(ctx.n_nodes()) + ", links=" +
                        std::to_string(ctx.n_links()) + ", mesh=" + std::to_string(mesh.n_cells()) +
                        ", stageCells=" + std::to_string(stage.cells().size()) + ", match=" +
                        std::to_string(
                            stage.matches(&mesh, &router.state(), &router.subsurface())) +
                        ", infilRows=" + std::to_string(router.infil().resolvedRows().size()) +
                        ", recharge=" + std::to_string(router.subcatchRecharge().size()) +
                        ", quality=" + std::to_string(router.surfaceQuality().active()) + "]");
        }
    }
    /**
     * @brief Configure a native hotstart FILES path before starting the default thermal profile.
     *
     * Does not clear the opposite path or other stored profile options. Calls start(name), so intended
     * use is a fresh compatible session and downstream errors propagate. Save output publication occurs
     * later through engine lifecycle, not in this setter.
     *
     * @param[in] name Raw thermal fixture artifact stem forwarded to start.
     * @param[in] path Hotstart path copied into the selected save/use member; written unescaped inside
     *     deck quotes.
     * @param[in] save True sets native_hotstart_save; false sets native_hotstart_use.
     */
    void startNativeHotstart(const std::string& name, const std::string& path, bool save) {
        if (save)
            native_hotstart_save = path;
        else
            native_hotstart_use = path;
        start(name);
    }
    /**
     * @brief Queue native hotstart application through the public API in default thermal setup.
     *
     * start opens/applies/closes the hotstart after initialize and before engine start. Does not clear
     * previously authored save/use paths or other flags; failures propagate as runtime_error and member
     * ownership supplies cleanup.
     *
     * @param[in] name Raw evidence stem forwarded to start.
     * @param[in] path Existing native hotstart filename copied into native_hotstart_apply.
     */
    void startNativeHotstartApi(const std::string& name, const std::string& path) {
        native_hotstart_apply = path;
        start(name);
    }
};
/**
 * @brief Start a mapped SIGMA groundwater/thermal profile with independently selected layer counts.
 *
 * Sets a cross-midnight three-minute calendar with seven-second reports, layered unsaturated
 * transport, sorption and a heterogeneous layer seed. Invokes ThermalSurfaceSession with SIGMA,
 * lateral conduction, heterogeneous ground and groundwater quality enabled. Raw authoring is
 * unescaped; downstream lifecycle/config failures propagate with partial owned state retained.
 *
 * @param[in,out] session Fresh owned thermal session; extra_input is replaced before start.
 * @param[in] name Raw evidence stem below root.
 * @param[in] chemistry Authored M_LAYERS chemistry discretization; must support the explicit
 *     layer-two initial row.
 * @param[in] thermal Authored THERMAL_LAYERS count, copied without range validation.
 * @param[in] output Enable FLOAT64 depth/heat/GW HDF5 output checkpointing.
 * @param[in] network_solver Raw optional QUALITY_SOLVER keyword; nonempty also includes the mapped
 *     1D channel.
 * @param[in] checkpoint Enable attached checkpointing through thermal setup when true.
 * @param[in] edge_boundary Author an aquifer flow edge with Dye/temperature/age boundary quality
 *     when true.
 */
void startMappedSigmaSession(ThermalSurfaceSession& session, const std::string& name,
                             int chemistry = 5, int thermal = 3, bool output = false,
                             const std::string& network_solver = "", bool checkpoint = true,
                             bool edge_boundary = false) {
    session.extra_input =
        "\n[OPTIONS]\nSTART_DATE 01/31/2026\nSTART_TIME 23:59:30\n"
        "END_DATE 02/01/2026\nEND_TIME 00:02:30\n"
        "REPORT_START_DATE 01/31/2026\nREPORT_START_TIME 23:59:30\nREPORT_STEP 00:00:07\n"
        "[2D_AQUIFER_OPTIONS]\nM_LAYERS " +
        std::to_string(chemistry) +
        "\n[GW_TRANSPORT_OPTIONS]\nUNSAT_TRANSPORT LAYERED\nTHERMAL_LAYERS " +
        std::to_string(thermal) +
        "\n[GW_SORPTION]\n* Dye 2 0\n"
        "[GW_INITIAL_QUALITY]\n* LAYER 2 Dye 23\n";
    if (edge_boundary)
        session.extra_input +=
            "[2D_VERTICES]\n0 0 .5\n4 0 .5\n0 2 .5\n4 2 .5\n"
            "[2D_TRIANGLES]\n0 1 2 .03 .04\n1 3 2 .03 .03\n"
            "[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 1 FLOW CONST .000001\n"
            "[GW_BOUNDARY_QUALITY]\n1 1 Dye CONC 3\n1 1 __TEMPERATURE__ CONC -4\n1 1 __WATER_AGE__ CONC 2\n";
    if (output)
        session.extra_input += "[2D_OPTIONS]\nREPORT_2D_VARIABLES DEPTH HEAT GROUNDWATER\n";
    if (!network_solver.empty())
        session.extra_input += "[OPTIONS]\nQUALITY_SOLVER " + network_solver + "\n";
    session.start(name, 12, "channel:test", false, false, false, true, true,
                  !network_solver.empty(), checkpoint, false, false, output, false, false, true,
                  "SIGMA");
}
}

TEST(EngineCheckpoint, SurfaceSessionRestoresActualEngineRainfallPidLidAndPendingBatches) {
    for (int lid = 0; lid < 4; ++lid)
        for (int mapped = 0; mapped < 3; ++mapped)
            for (int closure = 0; closure < 2; ++closure) {
                auto name = "surface-session-" + std::to_string(lid) + "-" +
                            std::to_string(mapped) + "-" + std::to_string(closure);
                SCOPED_TRACE(name);
                SurfaceSession a, b;
                ASSERT_NO_THROW(a.start(name + "-a", lid, mapped, closure, mapped));
                ASSERT_NO_THROW(b.start(name + "-b", lid, mapped, closure, mapped));
                while (a.engine.context().current_time < 2.1)
                    ASSERT_NO_THROW(step(a.engine));
                if (closure)
                    a.engine.surfaceRouter2D().subsurface().bookLinkSeepage(
                        0, .01); // Accepted recharge, not a live provider.
                while (a.engine.context().current_time < 43.1)
                    ASSERT_NO_THROW(step(a.engine));
                const auto file = root() / (name + ".chk");
                ASSERT_EQ(a.engine.saveCheckpoint(file.string()), "")
                    << "surface stores=" << a.engine.context().mass_balance_2d.init_storage << ","
                    << a.engine.context().mass_balance_2d.final_storage
                    << " rain=" << a.engine.context().mass_balance_2d.rainfall_in
                    << " ET=" << a.engine.context().mass_balance_2d.evap_out
                    << " infil=" << a.engine.context().mass_balance_2d.infil_out
                    << " return=" << a.engine.context().mass_balance_2d.aquifer_in;
                ASSERT_GT(a.stage.intervals(), 0);
                EXPECT_LT(a.stage.completedEnd(), a.engine.context().current_time);
                ASSERT_EQ(b.engine.loadCheckpoint(file.string()), "");
                compare(a.engine, b.engine, name + "-restored");
                int count = 0;
                while (a.engine.context().elapsed_ms <
                       a.engine.context().options.totalDurationMs()) {
                    ASSERT_NO_THROW(step(a.engine));
                    ASSERT_NO_THROW(step(b.engine));
                    ASSERT_EQ(a.engine.context().current_time, b.engine.context().current_time);
                    if (++count % 17 == 0)
                        compare(a.engine, b.engine, name + "-continued");
                }
                compare(a.engine, b.engine, name + "-final");
                EXPECT_GT(a.stage.meshRain(), 0);
                EXPECT_GT(a.stage.meshEvaporation(), 0);
                EXPECT_GT(a.engine.context().mass_balance.runoff_rainfall, 0);
                EXPECT_EQ(a.stage.meshReturn(), b.stage.meshReturn());
                if (closure)
                    EXPECT_GT(a.stage.meshReturn(), 0);
            }
}

TEST(EngineCheckpoint, CoupledMeshAndGroundThermalStateContinuesAcrossRestart) {
    ThermalSurfaceSession a, b;
    ASSERT_NO_THROW(a.start("surface-thermal-restart-a"));
    ASSERT_NO_THROW(b.start("surface-thermal-restart-b"));
    auto& router = a.engine.surfaceRouter2D();
    ASSERT_TRUE(router.state().heat.enabled);
    ASSERT_TRUE(router.subsurface().heat().active());
    double ground_before = 0;
    for (const auto& cell : router.subsurface().heat().cells)
        for (double value : cell.column.energy)
            ground_before += value;
    while (a.engine.context().current_time < 43.1)
        ASSERT_NO_THROW(step(a.engine));
    auto& ground = router.subsurface().heat();
    double initial = 0;
    for (const auto& cell : ground.cells)
        for (double value : cell.column.energy)
            initial += value;
    ASSERT_GT(initial, 0);
    EXPECT_GT(std::abs(initial - ground_before), 1e-8);
    const auto file = root() / "surface-thermal-restart.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(file.string()), "");
    ASSERT_EQ(b.engine.loadCheckpoint(file.string()), "");
    compare(a.engine, b.engine, "surface-thermal-restart-restored");
    int count = 0;
    while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(a.engine));
        ASSERT_NO_THROW(step(b.engine));
        if (++count % 11 == 0)
            compare(a.engine, b.engine, "surface-thermal-restart-continued");
    }
    compare(a.engine, b.engine, "surface-thermal-restart-final");
    for (const auto& cell : ground.cells)
        ASSERT_EQ(cell.failed, 0) << cell.error;
}

TEST(EngineCheckpoint, WaterTableThermalStateContinuesAcrossRestart) {
    for (const auto* closure : {"CLOSED_FORM", "ENSLAVED", "SIGMA"}) {
        SCOPED_TRACE(closure);
        const std::string suffix = closure;
        ThermalSurfaceSession source, target;
        ASSERT_NO_THROW(source.start("ground-water-table-thermal-source-" + suffix, 12,
                                     "channel:test", true, false, false, false, false, false, true,
                                     false, false, false, false, false, false, suffix));
        ASSERT_NO_THROW(target.start("ground-water-table-thermal-target-" + suffix, 12,
                                     "channel:test", true, false, false, false, false, false, true,
                                     false, false, false, false, false, false, suffix));
        auto& source_router = source.engine.surfaceRouter2D();
        auto& source_gw = source_router.subsurface();
        auto& source_transport = source_gw.transport();
        ASSERT_TRUE(source_gw.heat().active());
        ASSERT_GE(source_transport.temp_row, 0);
        const int cell = 0, row = source_transport.temp_row;
        const double initial_table = source_gw.state().hg[cell];
        double initial_energy = 0;
        for (const auto& c : source_gw.heat().cells)
            initial_energy += std::accumulate(c.column.energy.begin(), c.column.energy.end(), 0.0);
        constexpr double volume = .002, temperature = 25.0;
        source_gw.bookLinkSeepage(cell, volume);
        source_gw.bookLinkSeepageMass(cell, row, volume * temperature);

        while (source.engine.context().current_time < 43.1)
            ASSERT_NO_THROW(step(source.engine));
        EXPECT_DOUBLE_EQ(source_gw.state().lacc[cell], 0);
        EXPECT_NEAR(source_gw.state().led_link, volume, 1e-12);
        EXPECT_GT(source_gw.state().hg[cell], initial_table);
        double accepted_energy = 0;
        for (const auto& c : source_gw.heat().cells)
            accepted_energy += std::accumulate(c.column.energy.begin(), c.column.energy.end(), 0.0);
        EXPECT_NEAR(accepted_energy - initial_energy,
                    volume * temperature * source_gw.heat().water_capacity,
                    1e-7 * std::max(1.0, std::abs(accepted_energy)));
        EXPECT_NEAR(source_gw.state().continuityResidual(), 0, 1e-9);

        const auto file = root() / ("ground-water-table-thermal-restart-" + suffix + ".chk");
        ASSERT_EQ(source.engine.saveCheckpoint(file.string()), "");
        ASSERT_EQ(target.engine.loadCheckpoint(file.string()), "");
        compare(source.engine, target.engine, "ground-water-table-thermal-restored-" + suffix);
        int count = 0;
        while (source.engine.context().elapsed_ms <
               source.engine.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(source.engine));
            ASSERT_NO_THROW(step(target.engine));
            if (++count % 11 == 0)
                compare(source.engine, target.engine,
                        "ground-water-table-thermal-continued-" + suffix);
        }
        compare(source.engine, target.engine, "ground-water-table-thermal-final-" + suffix);
        auto& resumed = target.engine.surfaceRouter2D().subsurface();
        EXPECT_DOUBLE_EQ(source_gw.state().led_link, resumed.state().led_link);
        EXPECT_DOUBLE_EQ(source_gw.state().hg[cell], resumed.state().hg[cell]);
        EXPECT_DOUBLE_EQ(source_gw.heat().cells[cell].advective_j,
                         resumed.heat().cells[cell].advective_j);
        EXPECT_NEAR(source_gw.state().continuityResidual(), 0, 1e-9);
        EXPECT_NEAR(resumed.state().continuityResidual(), 0, 1e-9);
    }
}

TEST(EngineCheckpoint, StandaloneLocalBedThermalStateContinuesAcrossRestart) {
    ThermalSurfaceSession a, b;
    ASSERT_NO_THROW(a.start("surface-local-bed-restart-a", 12, "channel:test", false, false, false,
                            false, false, false, true, false, false, false, true));
    ASSERT_NO_THROW(b.start("surface-local-bed-restart-b", 12, "channel:test", false, false, false,
                            false, false, false, true, false, false, false, true));
    auto& router = a.engine.surfaceRouter2D();
    ASSERT_TRUE(router.state().heat.enabled);
    ASSERT_TRUE(router.state().heat.bed);
    ASSERT_FALSE(router.state().heat.ground_column);
    ASSERT_FALSE(router.subsurface().active());
    auto& heat = router.state().heat;
    ASSERT_EQ(heat.aquifer_ground, nullptr);
    ASSERT_EQ(heat.bed_energy.size(), 2u);
    ASSERT_EQ(heat.bed_capacity.size(), 2u);
    // Seed a distinct accepted bed state so restore must recover the local
    // finite store, rather than merely reproduce its initial configuration.
    heat.bed_energy[0] = heat.bed_capacity[0] * 4.25;
    heat.refreshBedView(0);
    while (a.engine.context().current_time < 43.1)
        ASSERT_NO_THROW(step(a.engine));
    const auto file = root() / "surface-local-bed-restart.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(file.string()), "");
    ASSERT_EQ(b.engine.loadCheckpoint(file.string()), "");
    compare(a.engine, b.engine, "surface-local-bed-restart-restored");
    EXPECT_EQ(b.engine.surfaceRouter2D().state().heat.aquifer_ground, nullptr);
    EXPECT_EQ(a.engine.surfaceRouter2D().state().heat.bed_energy,
              b.engine.surfaceRouter2D().state().heat.bed_energy);
    int count = 0;
    while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(a.engine));
        ASSERT_NO_THROW(step(b.engine));
        if (++count % 11 == 0)
            compare(a.engine, b.engine, "surface-local-bed-restart-continued");
    }
    compare(a.engine, b.engine, "surface-local-bed-restart-final");
    EXPECT_EQ(a.engine.surfaceRouter2D().state().heat.bed_energy,
              b.engine.surfaceRouter2D().state().heat.bed_energy);
}

TEST(EngineCheckpoint, StandaloneLocalBedQualityStateContinuesAcrossRestart) {
    ThermalSurfaceSession a, b;
    ASSERT_NO_THROW(a.start("surface-local-bed-quality-restart-a", 12, "channel:test", false, false,
                            false, false, false, false, true, false, false, false, true, true));
    ASSERT_NO_THROW(b.start("surface-local-bed-quality-restart-b", 12, "channel:test", false, false,
                            false, false, false, false, true, false, false, false, true, true));
    auto& router = a.engine.surfaceRouter2D();
    ASSERT_TRUE(router.state().heat.enabled);
    ASSERT_TRUE(router.state().heat.bed);
    ASSERT_FALSE(router.state().heat.ground_column);
    ASSERT_FALSE(router.subsurface().active());
    const auto& tr = router.state().transport;
    const auto& heat = router.state().heat;
    ASSERT_EQ(tr.n_pollut, 1);
    ASSERT_EQ(tr.n_msx, 0);
    ASSERT_GE(tr.age_row, 0);
    ASSERT_GE(tr.temp_row, 0);
    ASSERT_EQ(heat.bed_temp_row, tr.temp_row);
    ASSERT_EQ(heat.bed_age_row, tr.age_row);
    const int dye = tr.rowIndex("Dye");
    ASSERT_GE(dye, 0);
    ASSERT_EQ(heat.bed_carried_mass.size(), std::size_t(tr.n_species * tr.n_cells));
    const auto dyeMass = [&](const SWMMEngine& engine) {
        const auto& r = engine.surfaceRouter2D();
        const auto& s = r.state();
        const auto row = s.transport.rowIndex("Dye");
        double value = 0;
        for (int cell = 0; cell < r.mesh().n_cells(); ++cell)
            value += s.heat.bed_carried_mass[std::size_t(row * r.mesh().n_cells() + cell)];
        return value;
    };
    const auto ageMass = [&](const SWMMEngine& engine) {
        const auto& r = engine.surfaceRouter2D();
        const auto& s = r.state();
        double value = 0;
        for (int cell = 0; cell < r.mesh().n_cells(); ++cell)
            value +=
                s.heat
                    .bed_carried_mass[std::size_t(s.heat.bed_age_row * r.mesh().n_cells() + cell)];
        return value;
    };
    ASSERT_GT(dyeMass(a.engine), 0);
    const double age_initial = ageMass(a.engine);
    while (a.engine.context().current_time < 43.1)
        ASSERT_NO_THROW(step(a.engine));
    EXPECT_NE(ageMass(a.engine), age_initial);
    double generated_age = 0;
    for (double value : a.engine.surfaceRouter2D().state().heat.bed_generated_age)
        generated_age += value;
    EXPECT_GT(generated_age, 0);
    ASSERT_NE(a.engine.surfaceRouter2D().state().transport.gained_bed[dye], 0);
    const auto checkpoint = root() / "surface-local-bed-quality-restart.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
    ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
    compare(a.engine, b.engine, "surface-local-bed-quality-restart-restored");
    EXPECT_EQ(a.engine.surfaceRouter2D().state().heat.bed_carried_mass,
              b.engine.surfaceRouter2D().state().heat.bed_carried_mass);
    EXPECT_EQ(a.engine.surfaceRouter2D().state().transport.gained_bed,
              b.engine.surfaceRouter2D().state().transport.gained_bed);
    int count = 0;
    while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(a.engine));
        ASSERT_NO_THROW(step(b.engine));
        if (++count % 11 == 0)
            compare(a.engine, b.engine, "surface-local-bed-quality-restart-continued");
    }
    compare(a.engine, b.engine, "surface-local-bed-quality-restart-final");
    EXPECT_EQ(a.engine.surfaceRouter2D().state().heat.bed_carried_mass,
              b.engine.surfaceRouter2D().state().heat.bed_carried_mass);
    EXPECT_EQ(a.engine.surfaceRouter2D().state().transport.cell_mass,
              b.engine.surfaceRouter2D().state().transport.cell_mass);
    EXPECT_EQ(a.engine.surfaceRouter2D().state().transport.gained_bed,
              b.engine.surfaceRouter2D().state().transport.gained_bed);
}

TEST(EngineCheckpoint, GroundColumnOverlandPorewaterQualityContinuesAcrossRestart) {
    ThermalSurfaceSession a, b;
    ASSERT_NO_THROW(a.start("surface-ground-porewater-restart-a", 12, "channel:test", false, false,
                            false, false, false, false, true, false, false, false, false, false,
                            true));
    ASSERT_NO_THROW(b.start("surface-ground-porewater-restart-b", 12, "channel:test", false, false,
                            false, false, false, false, true, false, false, false, false, false,
                            true));
    const auto seed = [](SWMMEngine& engine) {
        auto& surface = engine.surfaceRouter2D().state();
        const int dye = surface.transport.rowIndex("Dye");
        if (dye < 0)
            throw std::runtime_error("2D restart fixture has no dye row.");
        for (int cell = 0; cell < surface.transport.n_cells; ++cell)
            surface.transport.cell_mass[surface.transport.idx(dye, cell)] = 0;
        auto& groundwater = engine.surfaceRouter2D().subsurface();
        auto& cell = groundwater.heat().cells[0];
        auto& pore = cell.porewater;
        const int ground_dye = groundwater.transport().rowIndex("Dye");
        const auto nl = cell.column.theta.size();
        if (ground_dye < 0 || nl == 0 || pore.retention.size() != std::size_t(pore.n_species) * nl)
            throw std::runtime_error("2D restart fixture ground porewater layout differs.");
        const double bed = engine.context().heat_config.sediment.bed_thickness;
        bool seeded = false;
        for (std::size_t layer = 0; layer < nl; ++layer) {
            const double overlap = std::max(0.0, std::min(cell.column.depth_edges[layer + 1], bed) -
                                                     cell.column.depth_edges[layer]);
            if (!(overlap > 0))
                continue;
            const double capacity =
                cell.column.theta[layer] * cell.column.area *
                    (cell.column.depth_edges[layer + 1] - cell.column.depth_edges[layer]) +
                pore.retention[std::size_t(ground_dye) * nl + layer];
            if (!(capacity > 0))
                throw std::runtime_error(
                    "2D restart fixture has no contacting porewater capacity.");
            pore.mass[std::size_t(ground_dye) * nl + layer] = 10 * capacity;
            seeded = true;
        }
        if (!seeded)
            throw std::runtime_error(
                "2D restart fixture bed does not intersect a groundwater layer.");
    };
    seed(a.engine);
    seed(b.engine);
    auto& router = a.engine.surfaceRouter2D();
    auto& ground = router.subsurface();
    const int dye = router.state().transport.rowIndex("Dye");
    ASSERT_TRUE(router.state().heat.ground_column);
    ASSERT_TRUE(router.state().heat.bed);
    ASSERT_EQ(router.state().heat.bed_species, router.state().transport.n_species);
    ASSERT_TRUE(ground.heat().active());
    ASSERT_GE(dye, 0);
    ASSERT_TRUE(ground.heat().cells[0].porewater.active());
    ASSERT_TRUE(router.state().heat.hyporheic_eligible[0]);
    while (a.engine.context().current_time < 3.1)
        ASSERT_NO_THROW(step(a.engine));
    ASSERT_EQ(router.state().heat.failures[0], 0);
    ASSERT_NE(router.state().transport.gained_bed[dye], 0)
        << "soil="
        << ground.heat()
               .cells[0]
               .porewater.mass[std::size_t(dye) * ground.heat().cells[0].column.theta.size()]
        << " surfaceVolume=" << router.state().volume[0]
        << " rate=" << router.state().heat.context->heat_config.sediment.hyporheic_velocity;
    ASSERT_NE(ground.transport().gained_contact[dye], 0);
    const auto& source_pore = ground.heat().cells[0].porewater;
    const auto checkpoint = root() / "surface-ground-porewater-restart.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
    ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
    auto& target_router = b.engine.surfaceRouter2D();
    auto& target_ground = target_router.subsurface();
    EXPECT_EQ(router.state().transport.gained_bed, target_router.state().transport.gained_bed);
    EXPECT_EQ(ground.transport().gained_contact, target_ground.transport().gained_contact);
    EXPECT_EQ(source_pore.mass, target_ground.heat().cells[0].porewater.mass);
    EXPECT_EQ(source_pore.contact_mass, target_ground.heat().cells[0].porewater.contact_mass);
    compare(a.engine, b.engine, "surface-ground-porewater-restored");
    while (a.engine.context().current_time < 20.1) {
        {
            ASSERT_NO_THROW(step(a.engine));
        }
        {
            ASSERT_NO_THROW(step(b.engine));
        }
    }
    EXPECT_EQ(router.state().transport.cell_mass, target_router.state().transport.cell_mass);
    EXPECT_EQ(router.state().transport.gained_bed, target_router.state().transport.gained_bed);
    EXPECT_EQ(ground.heat().cells[0].porewater.mass, target_ground.heat().cells[0].porewater.mass);
    EXPECT_EQ(ground.heat().cells[0].porewater.contact_mass,
              target_ground.heat().cells[0].porewater.contact_mass);
    EXPECT_EQ(ground.transport().gained_contact, target_ground.transport().gained_contact);
    EXPECT_NEAR(ground.transport().residual(dye), 0, 1e-9);
    EXPECT_NEAR(target_ground.transport().residual(dye), 0, 1e-9);
}

TEST(EngineCheckpoint, MappedSigmaChemistryContinuesFullSessionAcrossMidnightWithPendingContact) {
    for (double boundary : {7.1, 31.1}) {
        const auto name = "mapped-session-" + std::to_string(boundary);
        SCOPED_TRACE(name);
        ThermalSurfaceSession source, target;
        ASSERT_NO_THROW(startMappedSigmaSession(source, name + "-a"));
        ASSERT_NO_THROW(startMappedSigmaSession(target, name + "-b"));
        auto& router = source.engine.surfaceRouter2D();
        auto& gw = router.subsurface();
        auto& tr = gw.transport();
        ASSERT_EQ(tr.n_layers, 5);
        ASSERT_EQ(gw.heat().cells[0].column.theta.size(), 3);
        ASSERT_TRUE(gw.heat().cells[0].porewater.sigma.active());
        const int dye = tr.rowIndex("Dye");
        ASSERT_GE(dye, 0);
        const auto surface_mass = [&] {
            double mass = 0;
            const auto& t = router.state().transport;
            for (int cell = 0; cell < t.n_cells; ++cell)
                mass += t.cell_mass[t.idx(dye, cell)];
            return mass;
        };
        const double initial_mass = tr.ledgeredStorage(dye) + surface_mass();
        const double initial_table = gw.state().hg[0];
        // An accepted, cold leakage packet moves the table before the saved
        // boundary; a second packet is still waiting at that boundary.
        std::vector<double> concentration(tr.n_species, 0);
        concentration[dye] = 7;
        concentration[tr.temp_row] = -4;
        concentration[tr.age_row] = 2;
        gw.bookLinkContact(0, .002, concentration, -.25, -.25);
        while (source.engine.context().current_time < boundary)
            ASSERT_NO_THROW(step(source.engine));
        ASSERT_NE(gw.state().hg[0], initial_table);
        ASSERT_NE(tr.gained_contact[dye], 0);
        gw.bookLinkContact(0, .00025, concentration, -.25, -.25);
        ASSERT_NE(gw.state().lacc[0], 0);
        const auto path = root() / (name + ".chk");
        ASSERT_EQ(source.engine.saveCheckpoint(path.string()), "");
        ASSERT_EQ(target.engine.loadCheckpoint(path.string()), "");
        compare(source.engine, target.engine, name + "-restored");
        while (source.engine.context().current_time < 83.1) {
            ASSERT_NO_THROW(step(source.engine));
            ASSERT_NO_THROW(step(target.engine));
            EXPECT_EQ(tr.layer_mass,
                      target.engine.surfaceRouter2D().subsurface().transport().layer_mass);
        }
        compare(source.engine, target.engine, name + "-continued");
        EXPECT_EQ(gw.state().lacc[0], 0);
        EXPECT_NEAR(tr.gained_contact[dye] + router.state().transport.gained_bed[dye], 0,
                    1e-10 * std::max(1., initial_mass));
        EXPECT_NEAR(tr.ledgeredStorage(dye) + surface_mass(), initial_mass + .00225 * 7,
                    1e-10 * std::max(1., initial_mass));
        EXPECT_LE(std::abs(tr.residual(dye)), 1e-10 * std::max(1., tr.continuityDenominator(dye)));
        EXPECT_LE(std::abs(tr.residual(tr.age_row)),
                  1e-10 * std::max(1., tr.continuityDenominator(tr.age_row)));
        EXPECT_LE(std::abs(gw.state().continuityResidual()),
                  1e-10 * std::max(1., gw.state().led_init_storage));
        double thermal_residual[2]{};
        ASSERT_EQ(swmm_heat_get_ground_values(&source.engine, SWMM_GROUND_HEAT_NORMALIZED_RESIDUAL,
                                              0, 2, thermal_residual),
                  SWMM_OK);
        for (double value : thermal_residual)
            EXPECT_NEAR(value, 0, 1e-10);
    }
}

TEST(EngineCheckpoint, MappedSigmaNetworkChemistryKeepsUnqualifiedSessionGuard) {
    for (const std::string solver : {"LEGACY", "EULERIAN_ARD", "LAGRANGIAN"}) {
        SCOPED_TRACE(solver);
        ThermalSurfaceSession session;
        ASSERT_NO_THROW(startMappedSigmaSession(session, "mapped-network-guard-" + solver, 5, 3,
                                                false, solver, false));
        auto& engine = session.engine;
        auto& router = engine.surfaceRouter2D();
        ASSERT_TRUE(router.channelGround().active());
        const auto before = router.subsurface().transportRestartState();
        const auto nodes = engine.context().nodes.conc, links = engine.context().links.conc;
        const auto error = engine.enableCheckpointing(session.stage);
        EXPECT_NE(error.find("Hydraulic checkpoint quality"), std::string::npos) << error;
        EXPECT_EQ(router.subsurface().transportRestartState(), before);
        EXPECT_EQ(engine.context().nodes.conc, nodes);
        EXPECT_EQ(engine.context().links.conc, links);
        EXPECT_EQ(engine.context().current_time, 0);
        EXPECT_EQ(engine.checkpointSessionStatus() & SWMM_CHECKPOINT_SESSION_ATTACHED, 0u);
    }
}

TEST(EngineCheckpoint, MappedSigmaFullSessionRefusesChangedGridsAndTruncationWithoutMutation) {
    ThermalSurfaceSession source;
    ASSERT_NO_THROW(startMappedSigmaSession(source, "mapped-refusal-source"));
    while (source.engine.context().current_time < 31.1)
        ASSERT_NO_THROW(step(source.engine));
    const auto good = root() / "mapped-refusal-good.chk",
               bad = root() / "mapped-refusal-truncated.chk";
    ASSERT_EQ(source.engine.saveCheckpoint(good.string()), "");
    auto truncated = bytes(good);
    truncated.resize(truncated.size() - 7);
    write(bad, truncated);
    for (int variant = 0; variant < 3; ++variant) {
        const auto name = "mapped-refusal-" + std::to_string(variant);
        SCOPED_TRACE(name);
        ThermalSurfaceSession target;
        ASSERT_NO_THROW(
            startMappedSigmaSession(target, name, variant == 1 ? 6 : 5, variant == 2 ? 4 : 3));
        const auto before = root() / (name + "-before.chk"), after = root() / (name + "-after.chk");
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()), "");
        EXPECT_FALSE(target.engine.loadCheckpoint((variant == 0 ? bad : good).string()).empty());
        ASSERT_EQ(target.engine.saveCheckpoint(after.string()), "");
        EXPECT_EQ(bytes(before), bytes(after));
        EXPECT_EQ(target.engine.context().current_time, 0);
        if (variant == 0) {
            ASSERT_EQ(target.engine.loadCheckpoint(good.string()), "");
            compare(source.engine, target.engine, name + "-retry");
        }
    }
}

#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
namespace {
void compareMesh(SWMMEngine&, SWMMEngine&, const std::string&);
}
#endif
TEST(EngineCheckpoint, MeshRichardsLidThermalEnergyContinuesAcrossRestart) {
    ThermalSurfaceSession source, target;
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
    ASSERT_NO_THROW(source.start("surface-thermal-lid-source", 12, "channel:test", false, false,
                                 false, false, false, false, true, false, true, true, false, false,
                                 false, "CLOSED_FORM", true));
    ASSERT_NO_THROW(target.start("surface-thermal-lid-target", 12, "channel:test", false, false,
                                 false, false, false, false, true, false, true, true, false, false,
                                 false, "CLOSED_FORM", true));
#else
    ASSERT_NO_THROW(source.start("surface-thermal-lid-source", 12, "channel:test", false, false,
                                 false, false, false, false, true, false, true, false, false, false,
                                 "CLOSED_FORM", true));
    ASSERT_NO_THROW(target.start("surface-thermal-lid-target", 12, "channel:test", false, false,
                                 false, false, false, false, true, false, true, false, false, false,
                                 "CLOSED_FORM", true));
#endif
    const auto seed_lid = [](SWMMEngine& engine) {
        auto& ctx = engine.context();
        auto& router = engine.surfaceRouter2D();
        const int row = ctx.node_subtypes.storage_row(ctx.node_names.find("LID"));
        if (row < 0)
            throw std::runtime_error("Mesh LID checkpoint fixture has no storage owner.");
        auto& state = ctx.node_subtypes.storages.lid_state[std::size_t(row)];
        state.thermal_energy.clear();
        twoD::LidNodeThermalGraph graph;
        auto& ground = router.subsurface().heat();
        if (auto error = twoD::buildLidNodeThermalGraph(state, 0, ground.water_capacity,
                                                        ctx.heat_config.conduction, 30, graph);
            !error.empty())
            throw std::runtime_error(error);
        state.thermal_energy.assign(state.cells.size(), 0);
        for (std::size_t k = 0; k < graph.cells.size(); ++k)
            state.thermal_energy[graph.cells[k]] = graph.stores[k].capacity * 30;
    };
    seed_lid(source.engine);
    seed_lid(target.engine);
    auto& router = source.engine.surfaceRouter2D();
    const auto initial_lid_energy =
        source.engine.context()
            .node_subtypes.storages
            .lid_state[std::size_t(source.engine.context().node_subtypes.storage_row(
                source.engine.context().node_names.find("LID")))]
            .thermal_energy;
    ASSERT_TRUE(router.subsurface().heat().active());
    auto& context = source.engine.context();
    for (auto* engine : {&source.engine, &target.engine}) {
        auto& groundwater = engine->surfaceRouter2D().subsurface();
        groundwater.state().dt_cell[0] = 1;
        groundwater.state().dt_cell[1] = .2;
        groundwater.assignTiers(.2, 3);
        ASSERT_EQ(groundwater.state().tier[0], 2);
        ASSERT_EQ(groundwater.state().tier[1], 0);
    }
    const int row = context.node_subtypes.storage_row(context.node_names.find("LID"));
    ASSERT_GE(row, 0);
    auto& lid = context.node_subtypes.storages.lid_state[std::size_t(row)];
    ASSERT_TRUE(lid.richards);
    const auto pendingMagnitude = [](const twoD::SubsurfaceSolver& groundwater) {
        double value = 0;
        for (const auto& rows : groundwater.pendingLidReceipts())
            for (const auto& receipt : rows)
                value += std::abs(receipt.down) + std::abs(receipt.up);
        return value;
    };
    const auto mapped_cell = router.subsurface().nodeBeds().front().cell;
    ASSERT_GE(mapped_cell, 0);
    // Mesh-LID heat forces a groundwater co-advance on every accepted
    // routing step. A queued LID receipt is valid between cell firings, but
    // must be settled before the engine publishes a checkpoint boundary.
    while (source.engine.context().current_time < 43.1) {
        ASSERT_NO_THROW(step(source.engine));
        EXPECT_DOUBLE_EQ(pendingMagnitude(router.subsurface()), 0);
        EXPECT_DOUBLE_EQ(router.subsurface().cellCompletedTime(mapped_cell),
                         source.engine.context().current_time);
    }
    const double accepted_lid_water =
        std::accumulate(router.subsurface().richardsBedDownCumulative().begin(),
                        router.subsurface().richardsBedDownCumulative().end(), 0.0) +
        std::accumulate(router.subsurface().richardsBedUpCumulative().begin(),
                        router.subsurface().richardsBedUpCumulative().end(), 0.0);
    ASSERT_GT(accepted_lid_water, 0) << "No Richards water transfer was exercised.";
    auto& source_ground = router.subsurface().heat();
    ASSERT_LT(std::size_t(mapped_cell), source_ground.cells.size());
    ASSERT_TRUE(context.heat_config.layer_conduction);
    ASSERT_TRUE(source_ground.config && source_ground.config->options.conduction);
    ASSERT_TRUE(router.state().heat.enabled && router.state().heat.ground_column);
    ASSERT_EQ(source_ground.lid_owners[std::size_t(mapped_cell)].size(), 1u);
    ASSERT_EQ(lid.thermal_ground_contact_j.size(), lid.cells.size())
        << "surface temperature row=" << router.state().transport.temp_row
        << ", mesh heat=" << router.state().heat.enabled
        << ", mesh ground column=" << router.state().heat.ground_column
        << ", aquifer-owned=" << router.state().heat.aquifer_owned[std::size_t(mapped_cell)]
        << ", surface-owned=" << source_ground.cells[std::size_t(mapped_cell)].surface_owned
        << ", surface volume=" << router.state().volume[std::size_t(mapped_cell)]
        << ", cell failure=" << source_ground.cells[std::size_t(mapped_cell)].failed;
    const double lid_ground_contact_j = std::accumulate(lid.thermal_ground_contact_j.begin(),
                                                        lid.thermal_ground_contact_j.end(), 0.0);
    const double ground_lid_contact_j = source_ground.cells[std::size_t(mapped_cell)].lid_contact_j;
    ASSERT_GT(std::abs(ground_lid_contact_j), 0.0)
        << "The accepted Richards interval must exercise reciprocal LID-to-ground conduction.";
    EXPECT_NEAR(lid_ground_contact_j + ground_lid_contact_j, 0.0,
                1e-12 * std::max(1.0, std::abs(ground_lid_contact_j)));
    EXPECT_DOUBLE_EQ(pendingMagnitude(router.subsurface()), 0);
    EXPECT_DOUBLE_EQ(router.subsurface().cellCompletedTime(mapped_cell),
                     source.engine.context().current_time);
    ASSERT_FALSE(lid.thermal_energy.empty());
    EXPECT_NE(lid.thermal_energy, initial_lid_energy);
    std::size_t mapped_cells = 0;
    for (const auto& owners : router.subsurface().heat().lid_owners)
        if (!owners.empty())
            ++mapped_cells;
    EXPECT_EQ(mapped_cells, 1u);
    EXPECT_GT(std::accumulate(lid.thermal_energy.begin(), lid.thermal_energy.end(), 0.0), 0);
    const auto checkpoint = root() / "surface-thermal-lid-checkpoint.chk";
    ASSERT_EQ(source.engine.saveCheckpoint(checkpoint.string()), "");
    ASSERT_EQ(target.engine.loadCheckpoint(checkpoint.string()), "");
    auto& target_context = target.engine.context();
    const int target_row =
        target_context.node_subtypes.storage_row(target_context.node_names.find("LID"));
    ASSERT_GE(target_row, 0);
    const auto& target_lid =
        target_context.node_subtypes.storages.lid_state[std::size_t(target_row)];
    const auto& target_ground = target.engine.surfaceRouter2D().subsurface().heat();
    ASSERT_LT(std::size_t(mapped_cell), target_ground.cells.size());
    EXPECT_EQ(lid.quality_transfers.size(), target_lid.quality_transfers.size());
    EXPECT_EQ(lid.quality_ports.size(), target_lid.quality_ports.size());
    EXPECT_EQ(lid.richards_water, target_lid.richards_water);
    EXPECT_EQ(lid.quality_old_water, target_lid.quality_old_water);
    EXPECT_EQ(lid.thermal_energy, target_lid.thermal_energy);
    EXPECT_EQ(lid.thermal_ground_contact_j, target_lid.thermal_ground_contact_j);
    EXPECT_DOUBLE_EQ(source_ground.cells[std::size_t(mapped_cell)].lid_contact_j,
                     target_ground.cells[std::size_t(mapped_cell)].lid_contact_j);
    detail::CompletedSourceArchive source_groundwater_archive, target_groundwater_archive;
    router.subsurface().checkpoint(source_groundwater_archive, false);
    target.engine.surfaceRouter2D().subsurface().checkpoint(target_groundwater_archive, false);
    EXPECT_EQ(source_groundwater_archive.bytes(), target_groundwater_archive.bytes());
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
    compareMesh(source.engine, target.engine, "surface-thermal-lid-output-restored");
#else
    compare(source.engine, target.engine, "surface-thermal-lid-restored");
#endif
    EXPECT_EQ(
        lid.thermal_energy,
        target_context.node_subtypes.storages.lid_state[std::size_t(target_row)].thermal_energy);
    EXPECT_DOUBLE_EQ(pendingMagnitude(router.subsurface()), 0);
    EXPECT_DOUBLE_EQ(pendingMagnitude(target.engine.surfaceRouter2D().subsurface()), 0);
    int steps = 0;
    while (source.engine.context().elapsed_ms < source.engine.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(source.engine));
        ASSERT_NO_THROW(step(target.engine));
        if (++steps % 11 == 0)
            compare(source.engine, target.engine, "surface-thermal-lid-continued");
    }
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
    compareMesh(source.engine, target.engine, "surface-thermal-lid-output-final");
#else
    compare(source.engine, target.engine, "surface-thermal-lid-final");
#endif
    EXPECT_EQ(lid.thermal_ground_contact_j, target_lid.thermal_ground_contact_j);
    EXPECT_DOUBLE_EQ(source_ground.cells[std::size_t(mapped_cell)].lid_contact_j,
                     target_ground.cells[std::size_t(mapped_cell)].lid_contact_j);
}

TEST(EngineCheckpoint, DrySurfaceThermalStateContinuesAcrossRestart) {
    ThermalSurfaceSession source, target;
    ASSERT_NO_THROW(source.start("surface-thermal-dry-source", 12, "channel:test", true));
    ASSERT_NO_THROW(target.start("surface-thermal-dry-target", 12, "channel:test", true));
    for (int i = 0; i < 12; ++i)
        ASSERT_NO_THROW(step(source.engine));
    const auto& source_router = source.engine.surfaceRouter2D();
    ASSERT_DOUBLE_EQ(source_router.state().volume[0], 0);
    ASSERT_DOUBLE_EQ(source_router.state().volume[1], 0);
    double bed_energy = 0;
    for (double value : source_router.state().heat.bed_energy)
        bed_energy += value;
    double soil_energy = 0;
    for (const auto& cell : source_router.subsurface().heat().cells)
        for (double value : cell.column.energy)
            soil_energy += value;
    ASSERT_GT(std::abs(bed_energy) + std::abs(soil_energy), 0);

    const auto file = root() / "surface-thermal-dry.chk";
    ASSERT_EQ(source.engine.saveCheckpoint(file.string()), "");
    ASSERT_EQ(target.engine.loadCheckpoint(file.string()), "");
    compare(source.engine, target.engine, "surface-thermal-dry-restored");
    int count = 0;
    while (source.engine.context().elapsed_ms < source.engine.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(source.engine));
        ASSERT_NO_THROW(step(target.engine));
        if (++count % 9 == 0)
            compare(source.engine, target.engine, "surface-thermal-dry-continued");
    }
    compare(source.engine, target.engine, "surface-thermal-dry-final");
}

TEST(EngineCheckpoint, ThermalStateContinuesAcrossInMemoryClimateTransition) {
    ThermalSurfaceSession source, target;
    ASSERT_NO_THROW(
        source.start("surface-thermal-climate-source", 12, "channel:test", false, true, true));
    ASSERT_NO_THROW(
        target.start("surface-thermal-climate-target", 12, "channel:test", false, true, true));
    ASSERT_TRUE(source.engine.surfaceRouter2D().state().heat.surface);
    const double initial_air = source.engine.context().climate_state.temperature;
    while (source.engine.context().current_time < 24)
        ASSERT_NO_THROW(step(source.engine));
    EXPECT_NE(source.engine.context().climate_state.temperature, initial_air);
    EXPECT_DOUBLE_EQ(target.engine.context().current_time, 0);

    const auto file = root() / "surface-thermal-climate-transition.chk";
    ASSERT_EQ(source.engine.saveCheckpoint(file.string()), "");
    ASSERT_EQ(target.engine.loadCheckpoint(file.string()), "");
    compare(source.engine, target.engine, "surface-thermal-climate-transition-restored");
    int count = 0;
    while (source.engine.context().elapsed_ms < source.engine.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(source.engine));
        ASSERT_NO_THROW(step(target.engine));
        if (++count % 9 == 0)
            compare(source.engine, target.engine, "surface-thermal-climate-transition-continued");
    }
    compare(source.engine, target.engine, "surface-thermal-climate-transition-final");
}

TEST(EngineCheckpoint, ThermalPendingLinkReceiptSurvivesRestart) {
    ThermalSurfaceSession source, target;
    ASSERT_NO_THROW(source.start("surface-thermal-pending-source"));
    ASSERT_NO_THROW(target.start("surface-thermal-pending-target"));
    while (source.engine.context().current_time < 43.1)
        ASSERT_NO_THROW(step(source.engine));

    // Model a channel seepage packet accepted between aquifer firings. The
    // hydraulic volume and its signed temperature-volume must remain paired
    // through the checkpoint and be consumed once by the resumed cell clock.
    auto& gw = source.engine.surfaceRouter2D().subsurface();
    auto& transport = gw.transport();
    ASSERT_GE(transport.temp_row, 0);
    const int cell = 0;
    const double volume = 2.5e-5, temperature = 27.0;
    gw.bookLinkSeepage(cell, volume);
    gw.bookLinkSeepageMass(cell, transport.temp_row, volume * temperature);
    EXPECT_DOUBLE_EQ(gw.state().lacc[cell], volume);
    EXPECT_DOUBLE_EQ(transport.lacc_mass[transport.idx(transport.temp_row, cell)],
                     volume * temperature);

    const auto file = root() / "surface-thermal-pending-receipt.chk";
    ASSERT_EQ(source.engine.saveCheckpoint(file.string()), "");
    ASSERT_EQ(target.engine.loadCheckpoint(file.string()), "");
    auto& restored_gw = target.engine.surfaceRouter2D().subsurface();
    ASSERT_DOUBLE_EQ(restored_gw.state().lacc[cell], volume);
    ASSERT_DOUBLE_EQ(
        restored_gw.transport()
            .lacc_mass[restored_gw.transport().idx(restored_gw.transport().temp_row, cell)],
        volume * temperature);
    compare(source.engine, target.engine, "surface-thermal-pending-restored");

    int count = 0;
    while (source.engine.context().elapsed_ms < source.engine.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(source.engine));
        ASSERT_NO_THROW(step(target.engine));
        if (++count % 9 == 0)
            compare(source.engine, target.engine, "surface-thermal-pending-continued");
    }
    compare(source.engine, target.engine, "surface-thermal-pending-final");
    const auto& source_gw = source.engine.surfaceRouter2D().subsurface().state();
    const auto& target_gw = target.engine.surfaceRouter2D().subsurface().state();
    EXPECT_NEAR(source_gw.led_link, volume, 1e-12);
    EXPECT_DOUBLE_EQ(source_gw.led_link, target_gw.led_link);
    const auto& source_heat = source.engine.surfaceRouter2D().subsurface().heat();
    const auto& target_heat = target.engine.surfaceRouter2D().subsurface().heat();
    EXPECT_DOUBLE_EQ(source_heat.cells[cell].advective_j, target_heat.cells[cell].advective_j);
}

TEST(EngineCheckpoint, ThermalCrossTierContactReceiptSurvivesRestart) {
    ThermalSurfaceSession source, target;
    ASSERT_NO_THROW(source.start("surface-thermal-cross-tier-source", 12, "channel:test", false,
                                 false, false, true, true));
    ASSERT_NO_THROW(target.start("surface-thermal-cross-tier-target", 12, "channel:test", false,
                                 false, false, true, true));
    auto& gw = source.engine.surfaceRouter2D().subsurface();
    ASSERT_EQ(gw.state().n_cells, 2);
    const auto total = [](const auto& cells) {
        double value = 0;
        for (const auto& cell : cells)
            for (double energy : cell.column.energy)
                value += energy;
        return value;
    };
    const double initial = total(gw.heat().cells);
    gw.state().dt_cell[0] = .1;
    gw.state().dt_cell[1] = .8;
    gw.assignTiers(.1, 4);
    ASSERT_EQ(gw.state().tier[0], 0);
    ASSERT_EQ(gw.state().tier[1], 3);
    gw.fireGwFaces(0, .1);
    const double left_pending = gw.pendingHeat(0), right_pending = gw.pendingHeat(1);
    ASSERT_GT(std::abs(left_pending) + std::abs(right_pending), 0);
    EXPECT_NEAR(total(gw.heat().cells) + left_pending + right_pending, initial, 1e-5);

    const auto file = root() / "surface-thermal-cross-tier-pending.chk";
    ASSERT_EQ(source.engine.saveCheckpoint(file.string()), "");
    ASSERT_EQ(target.engine.loadCheckpoint(file.string()), "");
    auto& restored = target.engine.surfaceRouter2D().subsurface();
    EXPECT_DOUBLE_EQ(restored.pendingHeat(0), left_pending);
    EXPECT_DOUBLE_EQ(restored.pendingHeat(1), right_pending);
    compare(source.engine, target.engine, "surface-thermal-cross-tier-restored");

    int count = 0;
    while (source.engine.context().elapsed_ms < source.engine.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(source.engine));
        ASSERT_NO_THROW(step(target.engine));
        if (++count % 9 == 0)
            compare(source.engine, target.engine, "surface-thermal-cross-tier-continued");
    }
    compare(source.engine, target.engine, "surface-thermal-cross-tier-final");
}

TEST(EngineCheckpoint, RoutedChannelHyporheicThermalStateContinuesAcrossEngineCheckpoint) {
    ThermalSurfaceSession source, target;
    ASSERT_NO_THROW(source.start("surface-thermal-channel-source", 12, "channel:test", false, false,
                                 false, false, false, true, true));
    ASSERT_NO_THROW(target.start("surface-thermal-channel-target", 12, "channel:test", false, false,
                                 false, false, false, true, true));
    ASSERT_TRUE(source.engine.surfaceRouter2D().channelGround().active());

    while (source.engine.context().current_time < 43.1)
        ASSERT_NO_THROW(step(source.engine));
    const auto& channel = source.engine.surfaceRouter2D().channelGround();
    ASSERT_GT(channel.contactCount(0), 0u);
    double soil_exchange = 0;
    for (double value : channel.channelToGround())
        soil_exchange += value;
    ASSERT_GT(std::abs(soil_exchange), 0);
    ASSERT_EQ(source.engine.context().heat_state.mesh_contact_link,
              target.engine.context().heat_state.mesh_contact_link);
    ASSERT_EQ(source.engine.context().heat_state.link_temp.size(), 1u);

    const auto checkpoint = root() / "surface-thermal-channel-checkpoint.chk";
    ASSERT_EQ(source.engine.saveCheckpoint(checkpoint.string()), "");
    ASSERT_EQ(target.engine.loadCheckpoint(checkpoint.string()), "");
    compare(source.engine, target.engine, "surface-thermal-channel-restored");
    int count = 0;
    while (source.engine.context().elapsed_ms < source.engine.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(source.engine));
        ASSERT_NO_THROW(step(target.engine));
        if (++count % 13 == 0)
            compare(source.engine, target.engine, "surface-thermal-channel-continued");
    }
    compare(source.engine, target.engine, "surface-thermal-channel-final");
}

TEST(EngineCheckpoint, MeshThermalSourceIntakeUsesMixedSurfaceTemperature) {
    ThermalSurfaceSession session;
    ASSERT_NO_THROW(session.start("surface-thermal-mesh-source"));
    auto& surface = session.engine.surfaceRouter2D().state();
    auto& gw = session.engine.surfaceRouter2D().subsurface();
    ASSERT_EQ(surface.transport.temp_row, 0);
    ASSERT_EQ(surface.transport.n_species, 1);
    surface.rainfall[0] = .001;
    surface.transport.rain_conc = {5};

    ASSERT_EQ(session.stage.advance(0, 1), "");
    double intake_volume = 0, intake_temperature_volume = 0;
    for (const auto& receipt : session.stage.receipts())
        if (receipt.request.kind == twoD::SurfaceDonorKind::MESH) {
            intake_volume += receipt.volume;
            intake_temperature_volume += receipt.mass[0];
        }
    ASSERT_GT(intake_volume, 0);
    EXPECT_GT(intake_temperature_volume / intake_volume, 5);
    EXPECT_LT(intake_temperature_volume / intake_volume, 12);

    const auto initial_energy = [&]() {
        double total = 0;
        for (const auto& cell : gw.heat().cells)
            total += std::accumulate(cell.column.energy.begin(), cell.column.energy.end(), 0.);
        return total;
    };
    const double energy_before = initial_energy();
    gw.assignTiers(1, 1);
    gw.fireGwCells(0, 1, surface, 0, 1);
    for (const auto& cell : gw.heat().cells)
        ASSERT_EQ(cell.failed, 0) << cell.error;

    ASSERT_TRUE(gw.sourceReturns().empty());
    const double energy_change = initial_energy() - energy_before;
    double advective = 0;
    for (const auto& cell : gw.heat().cells)
        advective += cell.advective_j;
    EXPECT_NEAR(energy_change, advective, 1e-7 * std::max(1., std::abs(energy_change)));
    EXPECT_NEAR(advective, gw.heat().water_capacity * intake_temperature_volume,
                1e-7 * std::max(1., std::abs(advective)));

    ASSERT_EQ(session.stage.advance(1, 2), "");
    EXPECT_TRUE(gw.sourceReturns().empty());
    EXPECT_TRUE(session.stage.returns().empty());
}

TEST(EngineCheckpoint, MeshThermalRejectedIntakeRefundConservesEnergyAcrossLtsSourceIntervals) {
    ThermalSurfaceSession session;
    ASSERT_NO_THROW(session.start("surface-thermal-mesh-refund"));
    auto& surface = session.engine.surfaceRouter2D().state();
    auto& gw = session.engine.surfaceRouter2D().subsurface();
    ASSERT_EQ(surface.transport.temp_row, 0);
    surface.rainfall[0] = .001;
    surface.transport.rain_conc = {5};
    gw.state().dt_cell[0] = 1;
    gw.state().dt_cell[1] = .25;
    gw.assignTiers(.25, 3);
    ASSERT_EQ(gw.state().tier[0], 2);
    ASSERT_EQ(gw.state().tier[1], 0);

    const auto initial_energy = [&]() {
        double total = 0;
        for (const auto& cell : gw.heat().cells)
            total += std::accumulate(cell.column.energy.begin(), cell.column.energy.end(), 0.);
        return total;
    };
    const double energy_before = initial_energy();
    const double link_volume = 4, link_temperature = 27, cp = gw.heat().water_capacity;
    double intake_volume = 0, intake_temperature_volume = 0, cell0_intake_volume = 0,
           cell0_temperature_volume = 0;
    double refund_volume = 0, returned_temperature_volume = 0, refund_temperature_volume = 0;
    for (int substep = 0; substep < 4; ++substep) {
        const double start = .25 * substep, end = start + .25;
        ASSERT_EQ(session.stage.advance(start, end), "");
        for (const auto& receipt : session.stage.receipts())
            if (receipt.request.kind == twoD::SurfaceDonorKind::MESH) {
                intake_volume += receipt.volume;
                intake_temperature_volume += receipt.mass[0];
                if (receipt.request.cell == 0) {
                    cell0_intake_volume += receipt.volume;
                    cell0_temperature_volume += receipt.mass[0];
                }
            }
        if (substep == 0) {
            gw.bookLinkSeepage(0, link_volume);
            gw.bookLinkSeepageMass(0, surface.transport.temp_row, link_volume * link_temperature);
        }
        gw.fireGwCells(0, .25, surface, start, end);
        if (substep == 3)
            gw.fireGwCells(2, 1, surface, 0, end);
        for (const auto& cell : gw.heat().cells)
            ASSERT_EQ(cell.failed, 0) << cell.error;
        for (const auto& returned : gw.sourceReturns()) {
            returned_temperature_volume += returned.mass[0];
            if (returned.kind == twoD::SurfaceReturnKind::NUMERICAL_REFUND &&
                returned.intake.request.cell == 0) {
                refund_volume += returned.volume;
                refund_temperature_volume += returned.mass[0];
            }
        }
    }
    ASSERT_GT(intake_volume, 0);
    ASSERT_GT(refund_volume, 0) << "cell0 intake=" << cell0_intake_volume
                                << " rejected water=" << gw.state().led_reject
                                << " returns=" << gw.sourceReturns().size()
                                << " cell tiers=" << int(gw.state().tier[0]) << ","
                                << int(gw.state().tier[1]);
    ASSERT_GT(returned_temperature_volume, 0);
    EXPECT_NEAR(refund_volume, cell0_intake_volume, 1e-12);
    EXPECT_NEAR(refund_temperature_volume, cell0_temperature_volume, 1e-12);
    const double energy_after = initial_energy();
    const double external = cp * (intake_temperature_volume + link_volume * link_temperature);
    const double residual =
        energy_after - energy_before + cp * returned_temperature_volume - external;
    EXPECT_LE(std::abs(residual), 1e-8 * std::max(1., std::abs(external)))
        << "energy input=" << external << " J, residual=" << residual << " J";

    surface.rainfall[0] = 0;
    ASSERT_EQ(session.stage.advance(1, 1.25), "");
    EXPECT_TRUE(gw.sourceReturns().empty());
    ASSERT_FALSE(session.stage.returns().empty());
    EXPECT_NEAR(surface.transport.gained_exfiltration[0], returned_temperature_volume, 1e-12);
    double credited_temperature_volume = 0;
    for (const auto& returned : session.stage.returns())
        credited_temperature_volume += returned.mass[0];
    EXPECT_NEAR(credited_temperature_volume, returned_temperature_volume, 1e-12);
    ASSERT_EQ(session.stage.advance(1.25, 1.5), "");
    EXPECT_TRUE(session.stage.returns().empty());
}

TEST(EngineCheckpoint, MeshThermalIntakeRefundEnergyClosesAtGlobalAndLtsCadence) {
    struct Result {
        double energy_change = 0;
        double incoming_heat = 0;
        double returned_heat = 0;
        double credited_return = 0;
    };
    const auto run = [](bool staggered, const std::string& name) {
        ThermalSurfaceSession session;
        try {
            session.start(name);
        } catch (const std::exception& error) {
            ADD_FAILURE() << "thermal session setup failed: " << error.what();
            return Result{};
        }
        auto& surface = session.engine.surfaceRouter2D().state();
        auto& gw = session.engine.surfaceRouter2D().subsurface();
        EXPECT_EQ(surface.transport.temp_row, 0);
        surface.rainfall[0] = .001;
        surface.transport.rain_conc = {5};
        gw.state().dt_cell[0] = staggered ? 1 : .25;
        gw.state().dt_cell[1] = .25;
        gw.assignTiers(.25, 3);
        if (staggered) {
            EXPECT_EQ(gw.state().tier[0], 2);
            EXPECT_EQ(gw.state().tier[1], 0);
        } else {
            EXPECT_EQ(gw.state().tier[0], 0);
            EXPECT_EQ(gw.state().tier[1], 0);
        }

        const auto total_energy = [&]() {
            double total = 0;
            for (const auto& cell : gw.heat().cells)
                total += std::accumulate(cell.column.energy.begin(), cell.column.energy.end(), 0.);
            return total;
        };
        const double energy_before = total_energy();
        Result result;
        double intake_temperature_volume = 0;
        bool saw_numerical_refund = false;
        const double cp = gw.heat().water_capacity;
        for (int interval = 0; interval < 4; ++interval) {
            const double start = .25 * interval, end = start + .25;
            EXPECT_EQ(session.stage.advance(start, end), "");
            for (const auto& receipt : session.stage.receipts())
                if (receipt.request.kind == twoD::SurfaceDonorKind::MESH) {
                    intake_temperature_volume += receipt.mass[0];
                }
            // The same accepted link packet is spread over the four common
            // source intervals. LTS gathers it until its slower cell fires.
            gw.bookLinkSeepage(0, 1);
            gw.bookLinkSeepageMass(0, surface.transport.temp_row, 27);
            gw.fireGwCells(0, .25, surface, start, end);
            if (staggered && interval == 3)
                gw.fireGwCells(2, 1, surface, 0, end);
            for (const auto& cell : gw.heat().cells)
                EXPECT_EQ(cell.failed, 0) << cell.error;
            for (const auto& returned : gw.sourceReturns())
                saw_numerical_refund |= returned.kind == twoD::SurfaceReturnKind::NUMERICAL_REFUND;
        }
        EXPECT_TRUE(saw_numerical_refund);
        result.energy_change = total_energy() - energy_before;
        result.incoming_heat = cp * (intake_temperature_volume + 4 * 27);

        surface.rainfall[0] = 0;
        for (int interval = 0; interval < 8; ++interval) {
            const double start = 1 + .25 * interval, end = start + .25;
            EXPECT_EQ(session.stage.advance(start, end), "");
        }
        EXPECT_TRUE(gw.sourceReturns().empty());
        result.credited_return = surface.transport.gained_exfiltration[0];
        EXPECT_GT(result.credited_return, 0);
        result.returned_heat = cp * result.credited_return;
        EXPECT_EQ(session.stage.advance(3, 3.25), "");
        EXPECT_TRUE(session.stage.returns().empty());
        EXPECT_DOUBLE_EQ(surface.transport.gained_exfiltration[0], result.credited_return);
        return result;
    };

    const auto global = run(false, "surface-thermal-refund-global");
    const auto lts = run(true, "surface-thermal-refund-lts");
    for (const auto& result : {global, lts}) {
        const double residual = result.energy_change + result.returned_heat - result.incoming_heat;
        EXPECT_LE(std::abs(residual), 1e-8 * std::max(1., std::abs(result.incoming_heat)))
            << "energy input=" << result.incoming_heat << " J, residual=" << residual << " J";
    }
}

TEST(EngineCheckpoint, PendingThermalIntakeRefundSurvivesCommonSourceRestart) {
    ThermalSurfaceSession source, target;
    ASSERT_NO_THROW(source.start("surface-thermal-refund-restart-source"));
    ASSERT_NO_THROW(target.start("surface-thermal-refund-restart-target"));
    auto& source_surface = source.engine.surfaceRouter2D().state();
    auto& source_gw = source.engine.surfaceRouter2D().subsurface();
    source_surface.rainfall[0] = .001;
    ASSERT_EQ(source.stage.advance(0, 1), "");
    ASSERT_FALSE(source.stage.receipts().empty());
    source_gw.bookLinkSeepage(0, 4);
    source_gw.bookLinkSeepageMass(0, source_surface.transport.temp_row, 4 * 27);
    source_gw.assignTiers(1, 1);
    source_gw.fireGwCells(0, 1, source_surface, 0, 1);
    ASSERT_FALSE(source_gw.sourceReturns().empty());
    bool has_thermal_refund = false;
    for (const auto& returned : source_gw.sourceReturns())
        if (returned.kind == twoD::SurfaceReturnKind::NUMERICAL_REFUND && returned.mass[0] != 0)
            has_thermal_refund = true;
    ASSERT_TRUE(has_thermal_refund);

    const auto checkpoint = root() / "surface-thermal-pending-refund.source-checkpoint";
    ASSERT_EQ(source.stage.saveCheckpoint(checkpoint.string()), "");
    ASSERT_EQ(target.stage.loadCheckpoint(checkpoint.string()), "");
    auto& target_surface = target.engine.surfaceRouter2D().state();
    auto& target_gw = target.engine.surfaceRouter2D().subsurface();
    const auto source_copy =
        root() / "surface-thermal-pending-refund-source-copy.source-checkpoint";
    const auto target_copy =
        root() / "surface-thermal-pending-refund-target-copy.source-checkpoint";
    ASSERT_EQ(source.stage.saveCheckpoint(source_copy.string()), "");
    ASSERT_EQ(target.stage.saveCheckpoint(target_copy.string()), "");
    EXPECT_EQ(bytes(source_copy), bytes(target_copy));
    ASSERT_EQ(source_gw.sourceReturns().size(), target_gw.sourceReturns().size());
    for (std::size_t i = 0; i < source_gw.sourceReturns().size(); ++i) {
        EXPECT_EQ(source_gw.sourceReturns()[i].kind, target_gw.sourceReturns()[i].kind);
        EXPECT_DOUBLE_EQ(source_gw.sourceReturns()[i].available_at,
                         target_gw.sourceReturns()[i].available_at);
        EXPECT_DOUBLE_EQ(source_gw.sourceReturns()[i].volume, target_gw.sourceReturns()[i].volume);
        EXPECT_EQ(source_gw.sourceReturns()[i].mass, target_gw.sourceReturns()[i].mass);
    }

    ASSERT_EQ(source.stage.advance(1, 2), "");
    ASSERT_EQ(target.stage.advance(1, 2), "");
    EXPECT_TRUE(source_gw.sourceReturns().empty());
    EXPECT_TRUE(target_gw.sourceReturns().empty());
    ASSERT_FALSE(source.stage.returns().empty());
    ASSERT_EQ(source.stage.returns().size(), target.stage.returns().size());
    EXPECT_EQ(source_surface.transport.gained_exfiltration,
              target_surface.transport.gained_exfiltration);
    for (std::size_t i = 0; i < source.stage.returns().size(); ++i) {
        EXPECT_DOUBLE_EQ(source.stage.returns()[i].volume, target.stage.returns()[i].volume);
        EXPECT_EQ(source.stage.returns()[i].mass, target.stage.returns()[i].mass);
    }
    ASSERT_EQ(source.stage.advance(2, 3), "");
    ASSERT_EQ(target.stage.advance(2, 3), "");
    EXPECT_TRUE(source.stage.returns().empty());
    EXPECT_TRUE(target.stage.returns().empty());
    ASSERT_EQ(source.stage.saveCheckpoint(source_copy.string()), "");
    ASSERT_EQ(target.stage.saveCheckpoint(target_copy.string()), "");
    EXPECT_EQ(bytes(source_copy), bytes(target_copy));
}

TEST(EngineCheckpoint, ThermalCheckpointModelMismatchLeavesTargetThermalStateUnchanged) {
    ThermalSurfaceSession source, target;
    ASSERT_NO_THROW(source.start("surface-thermal-mismatch-source"));
    ASSERT_NO_THROW(target.start("surface-thermal-mismatch-target", 13));
    const auto sum = [](const auto& rows) {
        double value = 0;
        for (const auto& row : rows)
            value += row;
        return value;
    };
    const auto& before_mesh = target.engine.surfaceRouter2D().state().heat;
    const auto& before_ground = target.engine.surfaceRouter2D().subsurface().heat();
    const double bed_before = sum(before_mesh.bed_energy);
    double soil_before = 0;
    for (const auto& cell : before_ground.cells)
        soil_before += sum(cell.column.energy);
    for (int i = 0; i < 12; ++i)
        ASSERT_NO_THROW(step(source.engine));
    const auto file = root() / "surface-thermal-mismatch.chk";
    ASSERT_EQ(source.engine.saveCheckpoint(file.string()), "");

    EXPECT_FALSE(target.engine.loadCheckpoint(file.string()).empty());
    const auto& after_mesh = target.engine.surfaceRouter2D().state().heat;
    const auto& after_ground = target.engine.surfaceRouter2D().subsurface().heat();
    EXPECT_DOUBLE_EQ(sum(after_mesh.bed_energy), bed_before);
    double soil_after = 0;
    for (const auto& cell : after_ground.cells)
        soil_after += sum(cell.column.energy);
    EXPECT_DOUBLE_EQ(soil_after, soil_before);
    EXPECT_DOUBLE_EQ(target.engine.context().current_time, 0);
}

TEST(EngineCheckpoint, ThermalHyporheicTagMismatchLeavesTargetStateUnchanged) {
    ThermalSurfaceSession source, target;
    ASSERT_NO_THROW(source.start("surface-thermal-tag-source"));
    ASSERT_NO_THROW(target.start("surface-thermal-tag-target", 12, "channel:other"));
    for (int i = 0; i < 12; ++i)
        ASSERT_NO_THROW(step(source.engine));
    const auto file = root() / "surface-thermal-tag-mismatch.chk";
    ASSERT_EQ(source.engine.saveCheckpoint(file.string()), "");

    const auto& before = target.engine.surfaceRouter2D().state().heat;
    const auto& before_ground = target.engine.surfaceRouter2D().subsurface().heat();
    const auto bed = before.bed_energy;
    std::vector<double> soil;
    for (const auto& cell : before_ground.cells)
        soil.insert(soil.end(), cell.column.energy.begin(), cell.column.energy.end());
    EXPECT_FALSE(target.engine.loadCheckpoint(file.string()).empty());
    EXPECT_DOUBLE_EQ(target.engine.context().current_time, 0);
    EXPECT_EQ(target.engine.surfaceRouter2D().state().heat.bed_energy, bed);
    std::vector<double> after_soil;
    for (const auto& cell : target.engine.surfaceRouter2D().subsurface().heat().cells)
        after_soil.insert(after_soil.end(), cell.column.energy.begin(), cell.column.energy.end());
    EXPECT_EQ(after_soil, soil);
}

TEST(EngineCheckpoint, ThermalCheckpointCorruptionLeavesTargetUnchangedWithValidRetry) {
    ThermalSurfaceSession source, target;
    ASSERT_NO_THROW(source.start("surface-thermal-corrupt-source"));
    ASSERT_NO_THROW(target.start("surface-thermal-corrupt-target"));
    for (int i = 0; i < 12; ++i)
        ASSERT_NO_THROW(step(source.engine));
    const auto good = root() / "surface-thermal-corrupt-good.chk";
    const auto bad = root() / "surface-thermal-corrupt-bad.chk";
    ASSERT_EQ(source.engine.saveCheckpoint(good.string()), "");
    auto damaged = bytes(good);
    ASSERT_GT(damaged.size(), 1u);
    damaged.back() ^= 1;
    write(bad, damaged);

    const auto& before = target.engine.surfaceRouter2D().state().heat;
    const auto bed = before.bed_energy;
    double soil_before = 0;
    for (const auto& cell : target.engine.surfaceRouter2D().subsurface().heat().cells)
        for (double value : cell.column.energy)
            soil_before += value;
    EXPECT_FALSE(target.engine.loadCheckpoint(bad.string()).empty());
    EXPECT_DOUBLE_EQ(target.engine.context().current_time, 0);
    EXPECT_EQ(target.engine.surfaceRouter2D().state().heat.bed_energy, bed);
    double soil_after = 0;
    for (const auto& cell : target.engine.surfaceRouter2D().subsurface().heat().cells)
        for (double value : cell.column.energy)
            soil_after += value;
    EXPECT_DOUBLE_EQ(soil_after, soil_before);

    ASSERT_EQ(target.engine.loadCheckpoint(good.string()), "");
    compare(source.engine, target.engine, "surface-thermal-corrupt-valid-retry");
}

TEST(EngineCheckpoint, CoupledThermalStateRefusesIncompleteHotstartFormats) {
    ThermalSurfaceSession session;
    ASSERT_NO_THROW(session.start("surface-thermal-hotstart-refusal"));
    auto& ctx = session.engine.context();
    auto& router = session.engine.surfaceRouter2D();
    const double time_before = ctx.current_time;
    const auto bed_before = router.state().heat.bed_energy;
    std::vector<double> soil_before;
    for (const auto& cell : router.subsurface().heat().cells)
        soil_before.insert(soil_before.end(), cell.column.energy.begin(), cell.column.energy.end());

    const auto native_path = root() / "surface-thermal-refused-native.hsf";
    const auto legacy_path = root() / "surface-thermal-refused-legacy.hsf";
    std::error_code ec;
    std::filesystem::remove(native_path, ec);
    ec.clear();
    std::filesystem::remove(legacy_path, ec);

    EXPECT_EQ(HotStartManager::save(ctx, native_path.string()), nullptr);
    EXPECT_NE(HotStartManager::last_io_error().find("thermal energy"), std::string::npos);
    EXPECT_EQ(HotStartManager::save(ctx, nullptr, nullptr, native_path.string()), nullptr);
    EXPECT_NE(HotStartManager::last_io_error().find("thermal energy"), std::string::npos);
    HotStartFile native_file;
    EXPECT_EQ(HotStartManager::apply(native_file, ctx), -1);
    EXPECT_NE(HotStartManager::last_io_error().find("thermal energy"), std::string::npos);

    // The shared-source cell clocks require the V17 groundwater extension.
    // Legacy routing refuses that extension before examining the heat owner.
    EXPECT_EQ(HotStartManager::save_legacy_routing(legacy_path.string(), ctx), 1);
    EXPECT_NE(HotStartManager::last_io_error().find("groundwater"), std::string::npos);
    EXPECT_EQ(HotStartManager::apply_legacy_routing(legacy_path.string(), ctx), 1);
    EXPECT_NE(HotStartManager::last_io_error().find("groundwater"), std::string::npos);

    EXPECT_FALSE(std::filesystem::exists(native_path));
    EXPECT_FALSE(std::filesystem::exists(legacy_path));
    EXPECT_DOUBLE_EQ(ctx.current_time, time_before);
    EXPECT_EQ(router.state().heat.bed_energy, bed_before);
    std::vector<double> soil_after;
    for (const auto& cell : router.subsurface().heat().cells)
        soil_after.insert(soil_after.end(), cell.column.energy.begin(), cell.column.energy.end());
    EXPECT_EQ(soil_after, soil_before);
}

TEST(EngineCheckpoint, CoupledThermalNativeHotstartRestoresFullAcceptedEngineState) {
    const auto path = root() / "surface-thermal-native.hsf";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    ThermalSurfaceSession source, target, target_api;
    ASSERT_NO_THROW(
        source.startNativeHotstart("surface-thermal-native-source", path.string(), true));
    for (int i = 0; i < 12; ++i)
        ASSERT_NO_THROW(step(source.engine));
    const double saved_time = source.engine.context().current_time;
    ASSERT_EQ(swmm_hotstart_save(&source.engine, path.string().c_str()), SWMM_OK);
    {
        std::unique_ptr<HotStartFile> saved(HotStartManager::open(path.string()));
        ASSERT_TRUE(saved) << HotStartManager::last_io_error();
        EXPECT_EQ(saved->header.version, 17u);
        EXPECT_FALSE(saved->engine_checkpoint.empty());
    }

    ASSERT_NO_THROW(
        target.startNativeHotstart("surface-thermal-native-target", path.string(), false));
    ASSERT_NO_THROW(
        target_api.startNativeHotstartApi("surface-thermal-native-target-api", path.string()));
    EXPECT_DOUBLE_EQ(target.engine.context().current_time, saved_time);
    EXPECT_DOUBLE_EQ(target_api.engine.context().current_time, saved_time);
    compare(source.engine, target.engine, "surface-thermal-native-hotstart");
    compare(source.engine, target_api.engine, "surface-thermal-native-hotstart-api");
    for (int i = 0; i < 8; ++i) {
        ASSERT_NO_THROW(step(source.engine));
        ASSERT_NO_THROW(step(target.engine));
        ASSERT_NO_THROW(step(target_api.engine));
    }
    compare(source.engine, target.engine, "surface-thermal-native-hotstart-resume");
    compare(source.engine, target_api.engine, "surface-thermal-native-hotstart-api-resume");

    // [FILES] SAVE HOTSTART writes the same complete V17 state at end-of-run,
    // before teardown flushes a final partial mesh batch.
    ASSERT_EQ(source.engine.end(), SWMM_OK);
    std::unique_ptr<HotStartFile> scheduled(HotStartManager::open(path.string()));
    ASSERT_TRUE(scheduled) << HotStartManager::last_io_error();
    EXPECT_EQ(scheduled->header.version, 17u);
    EXPECT_FALSE(scheduled->engine_checkpoint.empty());
    ASSERT_EQ(target.engine.end(), SWMM_OK);
    ASSERT_EQ(target_api.engine.end(), SWMM_OK);
}

// A native hot start that restores a full engine checkpoint starts the run
// at the saved time. A dated SAVE HOTSTART row due before that time was the
// saving run's to write; it must not fire again on the first step and
// overwrite its file with later state. A row due after it still fires, and
// is written: a coupled-heat save needs the step's accepted checkpoint
// boundary, which dated rows used to fire before.
TEST(EngineCheckpoint, RestoredRunWritesOnlyTheDatedSavesStillAhead) {
    const auto path = root() / "surface-thermal-dated.hsf";
    const auto early = root() / "surface-thermal-dated-early.hsf";
    const auto late = root() / "surface-thermal-dated-late.hsf";
    std::error_code ec;
    for (const auto& p : {path, early, late})
        std::filesystem::remove(p, ec);
    ThermalSurfaceSession source, target;
    ASSERT_NO_THROW(
        source.startNativeHotstart("surface-thermal-dated-source", path.string(), true));
    for (int i = 0; i < 12; ++i)
        ASSERT_NO_THROW(step(source.engine));
    const double saved_ms = source.engine.context().elapsed_ms;
    ASSERT_GT(saved_ms, 5000.0);
    ASSERT_LT(saved_ms, 30000.0);
    ASSERT_EQ(swmm_hotstart_save(&source.engine, path.string().c_str()), SWMM_OK);

    target.extra_files = "SAVE HOTSTART \"" + early.string() +
                         "\" 01/01/2026 00:00:05\n"
                         "SAVE HOTSTART \"" +
                         late.string() + "\" 01/01/2026 00:00:30\n";
    ASSERT_NO_THROW(
        target.startNativeHotstart("surface-thermal-dated-target", path.string(), false));
    EXPECT_DOUBLE_EQ(target.engine.context().elapsed_ms, saved_ms);
    while (target.engine.context().elapsed_ms < 40000.0)
        ASSERT_NO_THROW(step(target.engine));
    EXPECT_FALSE(std::filesystem::exists(early))
        << "the row due before the restored time was written again";
    EXPECT_TRUE(std::filesystem::exists(late));
    ASSERT_EQ(target.engine.end(), SWMM_OK);
    for (const auto& w : target.engine.context().warnings) {
        EXPECT_EQ(w.find("was not written"), std::string::npos) << w;
        EXPECT_EQ(w.find("was not saved"), std::string::npos) << w; // needs the accepted boundary
    }
    ASSERT_EQ(source.engine.end(), SWMM_OK);
}

TEST(EngineCheckpoint, SurfaceAttachmentGuardsAreRetryableAtInitialBoundary) {
    SurfaceSession a, b;
    ASSERT_NO_THROW(a.start("surface-guard-a", 2, 1, 0, 0, false));
    ASSERT_NO_THROW(b.start("surface-guard-b", 2, 1, 0, 0, false));
    EXPECT_FALSE(a.engine.enableCheckpointing().empty());
    EXPECT_FALSE(a.engine.enableCheckpointing(b.stage).empty());
    auto& ctx = a.engine.context();
    ctx.events = {{ctx.options.start_date, datetime::addSeconds(ctx.options.start_date, 50)}};
    EXPECT_FALSE(a.engine.enableCheckpointing(a.stage).empty());
    ctx.events.clear();
    a.engine.surfaceRouter2D().options().output_file = "not-opened.h5";
    EXPECT_FALSE(a.engine.enableCheckpointing(a.stage).empty());
    a.engine.surfaceRouter2D().options().output_file = "";
    ASSERT_EQ(a.engine.enableCheckpointing(a.stage), "");
    ASSERT_EQ(b.engine.enableCheckpointing(b.stage), "");
    compare(a.engine, b.engine, "surface-initial");
    EXPECT_FALSE(a.engine.enableCheckpointing(a.stage).empty());
    ASSERT_NO_THROW(step(a.engine));
}

namespace {
/**
 * @brief Locate a known nested checkpoint blob around its first magic marker.
 *
 * Requires marker position >=16 and a bounded native uint64 span; throws runtime_error when those
 * checks fail. Uses the first raw match and assumes eight-byte native length layout, so arbitrary
 * payloads or coincidental markers are outside this fixture helper contract.
 *
 * @param[in] payload Borrowed known-layout parent archive bytes.
 * @param[in] magic Expected nested record marker searched as raw bytes.
 * @return Pair of parent offset at the outer uint64 length and an owned nested blob copy.
 */
std::pair<std::size_t, std::string> nested(const std::string& payload, const std::string& magic) {
    auto at = payload.find(magic);
    if (at == std::string::npos || at < 16)
        throw std::runtime_error("Missing nested checkpoint record");
    at -= 16;
    uint64_t size;
    std::memcpy(&size, payload.data() + at, 8);
    if (size > payload.size() - at - 8)
        throw std::runtime_error("Invalid nested checkpoint span");
    return {at, payload.substr(at + 8, size)};
}
/**
 * @brief Replace one known nested archive string in an owned payload copy.
 *
 * Does not recompute parent CRC/version metadata; caller wraps the changed payload later.
 * Missing/corrupt spans and allocation failures throw; caller inputs remain unchanged.
 *
 * @param[in] payload Owned-by-value parent archive bytes with the expected nested marker/layout.
 * @param[in] magic Exact raw marker forwarded to nested.
 * @param[in] replacement Borrowed replacement blob copied into a length-prefixed archive string.
 * @return Owned parent payload with the located nested member replaced.
 */
std::string replaceNested(std::string payload, const std::string& magic,
                          const std::string& replacement) {
    auto [at, old] = nested(payload, magic);
    detail::CompletedSourceArchive record;
    auto copy = replacement;
    record(copy);
    payload.replace(at, old.size() + 8, record.bytes());
    return payload;
}
/**
 * @brief Extract the joint DW surface/source record from an engine-session archive.
 *
 * CompletedSourceArchive checks decode/finish and nested checks marker/span; this helper does not
 * validate the outer magic/version/checksum itself. Malformed data throws; no state or file IO is
 * involved.
 *
 * @param[in] bytes Borrowed complete engine archive in the exact expected fixture field order.
 * @return Owned nested joint surface/source checkpoint blob.
 */
std::string engineSurface(const std::string& bytes) {
    detail::CompletedSourceArchive file(bytes);
    std::string magic, model, payload;
    uint32_t version, sum;
    file(magic, version, model, sum, payload);
    file.finish();
    return nested(payload, "OpenSWMM joint DW surface/source boundary").second;
}
/**
 * @brief Decode the payload field of a known surface checkpoint envelope.
 *
 * Requires expected field layout and no trailing bytes; does not independently validate
 * magic/version/endian/model/checksum values. Decode/allocation errors throw.
 *
 * @param[in] bytes Borrowed full envelope with
 *     magic/version/endian/long-width/model/payload/checksum fields.
 * @return Owned decoded payload bytes.
 */
std::string surfacePayload(const std::string& bytes) {
    detail::CompletedSourceArchive file(bytes);
    std::string magic, model, payload;
    uint32_t version, endian, longBytes, sum;
    file(magic, version, endian, longBytes, model, payload, sum);
    file.finish();
    return payload;
}
/**
 * @brief Re-encode a known surface envelope with a replacement payload and fresh CRC.
 *
 * Requires exact original envelope layout/finish. Does not verify its old checksum or validate
 * replacement contents; decode/allocation errors throw and no file is written.
 *
 * @param[in] bytes Borrowed original envelope supplying magic/version/endian/long-width/model
 *     metadata.
 * @param[in] payload Owned-by-value replacement payload, accepted without semantic validation.
 * @return Owned full envelope with metadata preserved and checksum recomputed.
 */
std::string withSurfacePayload(const std::string& bytes, std::string payload) {
    detail::CompletedSourceArchive file(bytes);
    std::string magic, model, old;
    uint32_t version, endian, longBytes, sum;
    file(magic, version, endian, longBytes, model, old, sum);
    file.finish();
    sum = crc(payload);
    detail::CompletedSourceArchive out;
    out(magic, version, endian, longBytes, model, payload, sum);
    return out.bytes();
}
}
TEST(EngineCheckpoint, SurfaceNestedFailuresPreserveEveryComponentAndAllowValidRetry) {
    SurfaceSession a;
    ASSERT_NO_THROW(a.start("surface-atomic-source", 2, 1));
    while (a.engine.context().current_time < 43.1)
        ASSERT_NO_THROW(step(a.engine));
    auto good = root() / "surface-atomic-good.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    const double savedStorage = a.engine.context().mass_balance.routing_final_storage;
    auto valid = bytes(good), surface = engineSurface(valid), data = surfacePayload(surface);
    detail::CompletedSourceArchive in(valid);
    std::string magic, model, payload;
    uint32_t version, sum;
    in(magic, version, model, sum, payload);
    in.finish();
    ASSERT_EQ(version, 104);
    a.engine.context().gages.rainfall[0] += 1;
    auto rain = root() / "surface-other-rain.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(rain.string()), "");
    auto otherRain = engineSurface(bytes(rain));
    a.engine.context().gages.rainfall[0] -= 1;
    for (int i = 0; i < 4; ++i)
        ASSERT_NO_THROW(step(a.engine));
    auto later = root() / "surface-other-time.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(later.string()), "");
    auto otherSurface = engineSurface(bytes(later)), otherData = surfacePayload(otherSurface);
    for (int k = 0; k < 9; ++k) {
        SCOPED_TRACE(k);
        SurfaceSession b;
        ASSERT_NO_THROW(b.start("surface-mutant-" + std::to_string(k), 2, 1));
        auto before = root() / "surface-atomic-before.chk",
             after = root() / "surface-atomic-after.chk", bad = root() / "surface-atomic-bad.chk";
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        auto changed = surface, body = payload;
        uint32_t v = version;
        if (k == 0)
            v = 3;
        if (k == 1)
            changed = otherSurface; // Individually valid, mismatched engine time.
        if (k == 2)
            changed = otherRain; // Individually valid, mismatched shared gage rainfall.
        if (k >= 3 && k <= 6) {
            const std::string tag = k % 2 ? "OpenSWMM CPU surface/source boundary"
                                          : "OpenSWMM accepted DW hydraulic boundary";
            auto replacement = nested(k <= 4 ? data : otherData, tag).second;
            if (k <= 4)
                replacement.back() ^= 1; // Outer checksums remain valid.
            changed = withSurfacePayload(surface, replaceNested(data, tag, replacement));
        }
        if (k == 7) {
            const auto offset = 7 * sizeof(double) + 6 * sizeof(double) + 2 * sizeof(int) +
                                4 * sizeof(bool) + 2 * sizeof(double) + 11 * sizeof(double) +
                                sizeof(uint64_t) + a.engine.context().n_nodes() * sizeof(double) +
                                23 * sizeof(double);
            double stored;
            std::memcpy(&stored, body.data() + offset, sizeof(stored));
            ASSERT_EQ(stored, savedStorage);
            stored += 1;
            std::memcpy(body.data() + offset, &stored, sizeof(stored));
        }
        body = replaceNested(body, "OpenSWMM joint DW surface/source boundary", changed);
        sum = crc(body);
        detail::CompletedSourceArchive out;
        out(magic, v, model, sum, body);
        auto mutant = out.bytes();
        if (k == 8)
            mutant.resize(mutant.size() / 2);
        write(bad, mutant);
        EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(b.engine.saveCheckpoint(after.string()), "");
        EXPECT_EQ(bytes(before), bytes(after));
        EXPECT_EQ(b.stage.completedEnd(), 0);
        EXPECT_EQ(b.stage.intervals(), 0);
        ASSERT_EQ(b.engine.loadCheckpoint(good.string()), "");
        // Compare against the saved boundary, before the source was advanced again.
        ASSERT_EQ(b.engine.saveCheckpoint(after.string()), "");
        EXPECT_EQ(bytes(good), bytes(after));
    }
}
TEST(EngineCheckpoint, SurfaceSessionRejectsComponentOnlyRestoreAndUnsupportedDonorAttachment) {
    twoD::SourceResultHistory history;
    SurfaceSession a;
    ASSERT_NO_THROW(a.start("surface-component-guard", 2, 2, 0, 0, false));
    runoff::SourceWaterDriver donors;
    ASSERT_EQ(donors.initialize(a.engine.context(), {0}), "");
    twoD::CommonSourceStage rejected;
    twoD::SurfaceOwnershipPreview preview;
    auto& r = a.engine.surfaceRouter2D();
    preview.mesh_weather_area = r.mesh().tri_area;
    EXPECT_FALSE(
        rejected.initialize(r.mesh(), r.state(), r.options(), r.subsurface(), donors, preview, true)
            .empty());
    EXPECT_FALSE(a.engine.enableCheckpointing(rejected).empty());
    ASSERT_EQ(a.engine.enableCheckpointing(a.stage), "");
    auto file = root() / "surface-component-guard.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(file.string()), "")
        << "surface stores=" << a.engine.context().mass_balance_2d.init_storage << ","
        << a.engine.context().mass_balance_2d.final_storage
        << " rain=" << a.engine.context().mass_balance_2d.rainfall_in
        << " ET=" << a.engine.context().mass_balance_2d.evap_out
        << " infil=" << a.engine.context().mass_balance_2d.infil_out
        << " return=" << a.engine.context().mass_balance_2d.aquifer_in;
    auto original = bytes(file);
    EXPECT_FALSE(r.saveCheckpoint(a.engine.context(), file.string()).empty());
    EXPECT_FALSE(r.loadCheckpoint(a.engine.context(), file.string()).empty());
    EXPECT_EQ(bytes(file), original);
    // Private, unadvanced catchments have no qualified source-result endpoints.
    EXPECT_THROW(r.setSourceResultHistory(history), std::invalid_argument);
    ASSERT_EQ(a.engine.saveCheckpoint(file.string()), "");
    EXPECT_EQ(bytes(file), original);
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, SpatialSourcesOwnGraphsDeliverWetWeatherAndContinueVariableEngineSteps) {
    for (int method = 0; method < 5; ++method)
        for (int rain = 0; rain < 3; ++rain)
            for (int lid = 0; lid < 4; ++lid) {
                const int mode = method + 5 * rain;
                auto name = "spatial-" + std::to_string(mode) + "-" + std::to_string(lid);
                SCOPED_TRACE(name);
                SurfaceSession a, b;
                ASSERT_NO_THROW(a.start(name + "-a", lid, rain % 3, method % 2, mode, true, true));
                ASSERT_NO_THROW(b.start(name + "-b", lid, rain % 3, method % 2, mode, true, true));
                const auto* bridge = a.engine.sourceDelivery();
                ASSERT_NE(bridge, nullptr);
                EXPECT_TRUE(bridge->ownsSubcatch(0));
                EXPECT_TRUE(bridge->ownsSubcatch(1));
                EXPECT_FALSE(bridge->ownsSubcatch(2));
                const auto initial = a.engine.lid().waterTrial();
                while (a.engine.context().current_time < 2.1)
                    ASSERT_NO_THROW(step(a.engine));
                const auto file = root() / (name + ".chk");
                ASSERT_EQ(a.engine.saveCheckpoint(file.string()), "");
                ASSERT_EQ(b.engine.loadCheckpoint(file.string()), "");
                compare(a.engine, b.engine, name + "-restored");
                double min_dt = 1e9, max_dt = 0;
                int steps = 0;
                bool runoff = false, drain = false, returning = false;
                while (a.engine.context().elapsed_ms <
                       a.engine.context().options.totalDurationMs()) {
                    const double before = a.engine.context().current_time;
                    ASSERT_NO_THROW(step(a.engine));
                    ASSERT_NO_THROW(step(b.engine));
                    const double dt = a.engine.context().current_time - before;
                    min_dt = std::min(min_dt, dt);
                    max_dt = std::max(max_dt, dt);
                    const auto& c = a.engine.context();
                    for (int sc : {0, 1}) {
                        EXPECT_DOUBLE_EQ(c.subcatches.runoff[sc], 0);
                        EXPECT_DOUBLE_EQ(c.subcatches.rainfall[sc], 0);
                        EXPECT_DOUBLE_EQ(c.subcatches.infil_loss[sc], 0);
                        EXPECT_DOUBLE_EQ(c.subcatches.lid_drain_flow[sc], 0);
                    }
                    EXPECT_NEAR(a.sources.balanceResidual(0), 0, 1e-6);
                    for (const auto& d : bridge->delivered()) {
                        runoff |= d.runoff > 0;
                        drain |= d.drain > 0;
                    }
                    for (int sc : {0, 1})
                        returning |= a.sources.ledgers()[sc].aquifer_return > 0;
                    if (++steps % 41 == 0)
                        compare(a.engine, b.engine, name + "-continued");
                }
                EXPECT_LT(min_dt, max_dt);
                EXPECT_TRUE(runoff);
                EXPECT_TRUE(drain);
                EXPECT_GT(a.engine.context().subcatches.stat_precip_vol[2], 0);
                EXPECT_GT(a.engine.context().mass_balance.runoff_rainfall, 0);
                EXPECT_GT(a.engine.context().mass_balance.routing_wet_weather, 0);
                EXPECT_EQ(a.sources.deliveries().size(), bridge->capturedCount());
                compare(a.engine, b.engine, name + "-final");
                EXPECT_EQ(a.sources.context().subcatches.runoff,
                          b.sources.context().subcatches.runoff);
                if (method % 2)
                    EXPECT_TRUE(returning);
                for (const auto& [t, u] : initial.usageOrder())
                    if (bridge->ownsSubcatch(initial.group(t).subcatch_idx[u])) {
                        const auto& g = a.engine.lid().group(t);
                        const auto& original = initial.group(t);
                        EXPECT_DOUBLE_EQ(g.surf_depth[u], original.surf_depth[u]);
                        EXPECT_DOUBLE_EQ(g.soil_moist[u], original.soil_moist[u]);
                        EXPECT_DOUBLE_EQ(g.stor_depth[u], original.stor_depth[u]);
                        EXPECT_DOUBLE_EQ(g.drain_flow[u], original.drain_flow[u]);
                        EXPECT_DOUBLE_EQ(g.wb_inflow[u], original.wb_inflow[u]);
                        EXPECT_DOUBLE_EQ(g.wb_infil[u], original.wb_infil[u]);
                    }
                a.engine.end();
                b.engine.end();
            }
}
TEST(EngineCheckpoint,
     SpatialAttachmentIsExplicitAndRefusesOwnedOutfallReturnsAndChangedSourceMaterials) {
    SurfaceSession a;
    ASSERT_NO_THROW(a.start("spatial-guard", 2, 0, 0, 0, false, true));
    EXPECT_FALSE(a.engine.enableCheckpointing(a.stage).empty());
    EXPECT_EQ(a.engine.sourceDelivery(), nullptr);
    a.engine.context().node_subtypes.outfalls.route_to[0] = 0;
    EXPECT_FALSE(a.engine.enableSpatialSourceCheckpointing(a.stage).empty());
    EXPECT_EQ(a.engine.sourceDelivery(), nullptr);
    a.engine.context().node_subtypes.outfalls.route_to[0] = 2;
    ASSERT_EQ(a.engine.enableSpatialSourceCheckpointing(a.stage), "");
    EXPECT_FALSE(a.engine.enableSpatialSourceCheckpointing(a.stage).empty());
    SurfaceSession b;
    ASSERT_NO_THROW(b.start("spatial-material", 2, 0, 0, 0, false, true));
    b.engine.context().subcatches.width[0] += 1;
    EXPECT_FALSE(b.engine.enableSpatialSourceCheckpointing(b.stage).empty());
    EXPECT_EQ(b.engine.sourceDelivery(), nullptr);
    b.engine.context().subcatches.width[0] -= 1;
    ASSERT_EQ(b.engine.enableSpatialSourceCheckpointing(b.stage), "");
    SurfaceSession c;
    ASSERT_NO_THROW(c.start("spatial-all-sources", 2, 0, 0, 0, false, true, true));
    c.engine.context().node_subtypes.outfalls.route_to[0] = 2;
    EXPECT_FALSE(c.engine.enableSpatialSourceCheckpointing(c.stage).empty());
    c.engine.context().node_subtypes.outfalls.route_to[0] = -1;
    ASSERT_EQ(c.engine.enableSpatialSourceCheckpointing(c.stage), "");
    auto invalid_lid = c.engine.lid().waterTrial();
    auto& g = invalid_lid.group(int(lid::LIDType::BIO_CELL));
    ASSERT_GT(g.count, 0);
    g.surf_void_frac[0] = 0;
    runoff::SourceWaterDriver refused;
    EXPECT_FALSE(refused.initialize(c.engine.context(), {0}, {}, &invalid_lid).empty());
    ASSERT_EQ(refused.initialize(c.engine.context(), {0}), "");
    ASSERT_NO_THROW(step(c.engine));
    EXPECT_FALSE(c.engine.saveCheckpoint((root() / "spatial-invalid-mode.chk").string()).size());
    ASSERT_EQ(c.engine.close(), 0);
    EXPECT_EQ(c.engine.sourceDelivery(), nullptr);
}
#endif

#ifdef OPENSWMM_HAS_2D
namespace {
/**
 * @brief Start a closed, reviewed spatial-source water-accounting checkpoint fixture.
 *
 * Builds a thirty-second CFS catchment/LID network over two dry SI cells with zero hydraulic
 * exchange coefficient, CLOSED_FORM aquifer and reviewed 0.5 m2 inside footprints. Prepares
 * selected output capabilities before start, initializes source driver/ownership stage and enables
 * spatial checkpointing. Throws on lifecycle/source/checkpoint rejection; unchecked deck writes and
 * raw parameters can fail downstream. Caller owns later stepping/cleanup and must use valid
 * positive cadence.
 *
 * @param[in,out] m Fresh session retaining source/stage members until after engine destruction.
 * @param[in] name Raw root-relative evidence stem; requested output artifacts can be
 *     removed/overwritten.
 * @param[in] node_lid 0 plain sink, nonzero NODE LID, exact 2 Richards LID flow.
 * @param[in] cadence Positive fixed routing interval in seconds.
 * @param[in] returns Track outfall returns routed to source zero when true.
 * @param[in] multiple Add DRAIN2 and a second subcatchment drain usage when true.
 * @param[in] output Enable binary saved results and output checkpoint preparation.
 * @param[in] mesh_output 0 none, 2 FLOAT32/compression4, 3 minimal variables, 4 late reporting,
 *     other nonzero FLOAT64.
 * @param[in] average Enable averaged reporting when true.
 */
void startClosedSpatial(SurfaceSession& m, const std::string& name, int node_lid = 0,
                        double cadence = .7, bool returns = false, bool multiple = false,
                        bool output = false, int mesh_output = 0, bool average = false) {
    auto path = root() / name;
    std::string deck =
        "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 10/10/2026\nSTART_TIME 00:00:00\nEND_DATE 10/10/2026\nEND_TIME 00:00:30\nROUTING_STEP " +
        std::to_string(cadence) +
        "\nVARIABLE_STEP 0\nINFILTRATION GREEN_AMPT\nRULE_STEP 00:00:02\nREPORT_STEP 00:00:03\nWET_STEP 00:00:01\nDRY_STEP 00:00:07\nIGNORE_QUALITY YES\nIGNORE_SNOWMELT YES\nIGNORE_GROUNDWATER YES\nIGNORE_2D NO\n[STORAGE]\nSINK 0 100 0 FUNCTIONAL 0 0 100 0 0\n[OUTFALLS]\nDRAIN 0 FREE NO\n[RAINGAGES]\nR INTENSITY 0:00:05 1 TIMESERIES Rain\nDry INTENSITY 0:00:05 1 TIMESERIES Zero\n[SUBCATCHMENTS]\nA R B .01 45 10 1 0\nB R SINK .02 65 10 1 0\nD Dry SINK .01 50 10 1 0\n[SUBAREAS]\nA .015 .12 .03 .05 35 PERVIOUS 55\nB .02 .13 .02 .04 25 OUTLET\nD .02 .13 .02 .04 25 OUTLET\n[INFILTRATION]\nA 3 .25 .3\nB 3 .25 .3\nD 3 .25 .3\n[LID_CONTROLS]\nI IT\nI SURFACE 3 .05 .12 1 3\nI STORAGE 6 .667 .1 0\nI DRAIN .5 .5 0 0\n[LID_USAGE]\nA I 1 20 3 70 30 0 * DRAIN\n[EVAPORATION]\nCONSTANT 0\n[TIMESERIES]\nRain 10/10/2026 00:00:00 5\nRain 10/10/2026 00:00:05 0\nZero 10/10/2026 00:00:00 0\n";
    if (node_lid) {
        deck.replace(deck.find("SINK 0 100 0"), 12, "SINK 0 2 0");
        deck +=
            "[LID_CONTROLS]\nStack NODE\nStack SURFACE 6 .1\nStack MEDIA 12 .45 .2 .08 2 10 3\nStack AGGREGATE 6 .4 100\nStack BOTTOM 0 0\n[LID_NODES]\nSINK Stack 10\n";
        if (node_lid == 2)
            deck +=
                "[LID_RICHARDS]\nStack OPTIONS 4 1e-7 1e-5 30\nStack 2 .03 2 1.6 .5 .0001\nStack 3 .03 2 1.6 .5 .0001\n";
    }
    if (multiple)
        deck += "[OUTFALLS]\nDRAIN2 0 FREE NO\n[LID_USAGE]\nB I 1 10 3 70 0 0 * DRAIN2\n";
    if (mesh_output) {
        std::filesystem::remove(path.string() + ".h5");
        deck += "[2D_OPTIONS]\nOUTPUT_FILE " + path.string() + ".h5\nOUTPUT_PRECISION " +
                (mesh_output == 2 ? std::string("FLOAT32") : std::string("FLOAT64")) +
                "\nOUTPUT_COMPRESSION " + (mesh_output == 2 ? std::string("4") : std::string("0")) +
                "\nREPORT_2D_STEP 00:00:06\n";
        if (mesh_output == 3)
            deck += "REPORT_2D_VARIABLES MINIMAL\n";
        if (mesh_output == 4)
            deck += "[OPTIONS]\nREPORT_START_DATE 10/10/2026\nREPORT_START_TIME 00:00:13\n";
    }
    write(path.string() + ".inp", deck);
    auto& e = m.engine;
    if (output)
        std::filesystem::remove(path.string() + ".out");
    if (e.open((path.string() + ".inp").c_str(), (path.string() + ".rpt").c_str(),
               output ? (path.string() + ".out").c_str() : nullptr))
        throw std::runtime_error(e.context().error_message);
    auto& router = e.surfaceRouter2D();
    auto& mesh = router.mesh();
    mesh.resize_vertices(4);
    mesh.resize_triangles(2);
    mesh.vx = {0, 2, 0, 2};
    mesh.vy = {0, 0, 2, 2};
    mesh.vz = {0, 0, 0, 0};
    mesh.set_triangle(0, 0, 1, 2);
    mesh.set_triangle(1, 1, 3, 2);
    mesh.mannings_n = {.03, .03};
    mesh.tri_init_depth = {0, 0};
    mesh.tri_coupled_node = {e.context().node_names.find("SINK"), -1};
    // Retain a joint coupling identity while closing hydraulic exchange.
    mesh.tri_coupling_cd = {0, 0};
    mesh.tri_coupling_area = {1, 1};
    auto& options = router.options();
    options.mesh_units_si = true;
    options.num_threads = 1;
    options.max_timestep = .21;
    options.coupling_sync = 3.5;
    options.evaporation = 2;
    options.rainfall_mode = twoD::RainfallMode::SYSTEM;
    auto& config = router.aquiferConfig();
    config.options.authored = true;
    config.options.force_closed_form = true;
    config.options.closure = twoD::GwClosure::CLOSED_FORM;
    twoD::GwAquiferRow row;
    row.Ks = .000001 * 3600 / .0254;
    row.zs = 2 / .3048;
    row.hg0 = 1 / .3048;
    row.alpha = 2 * .3048;
    config.rows.push_back(row);
    e.context().options.rpt_averages = average;
    if (e.initialize())
        throw std::runtime_error(e.context().error_message);
    if (output && mesh_output) {
        auto error = e.prepareCombinedOutputCheckpointing();
        if (!error.empty())
            throw std::runtime_error(error);
    } else if (output) {
        auto error = e.prepareOutputCheckpointing();
        if (!error.empty())
            throw std::runtime_error(error);
    } else if (mesh_output) {
        auto error = e.prepareSurfaceOutputCheckpointing();
        if (!error.empty())
            throw std::runtime_error(error);
    }
    if (e.start(output ? 1 : 0))
        throw std::runtime_error(e.context().error_message);
    if (returns)
        std::fill(e.context().node_subtypes.outfalls.route_to.begin(),
                  e.context().node_subtypes.outfalls.route_to.end(), 0);
    auto error = m.sources.initialize(e.context(), {0}, {}, nullptr, returns);
    if (!error.empty())
        throw std::runtime_error(error);
    twoD::SurfaceOwnershipPreview preview;
    preview.mesh_weather_area = mesh.tri_area;
    constexpr double ft2 = .3048 * .3048;
    for (int sc : {0, 1}) {
        const auto& r = m.sources.runoff().soa();
        const double lids = m.sources.context().subcatches.total_lid_area_ft2[sc] * ft2;
        const double full = r.area[sc] * ft2 + lids, inside = .5;
        twoD::SurfaceOwnerObject object;
        object.subcatch = sc;
        object.reviewed = true;
        object.declared_area = full;
        object.inside_area = inside;
        object.outside_area = full - inside;
        preview.objects.push_back(object);
        twoD::SurfaceOwnerShare share;
        share.subcatch = sc;
        share.cell = sc;
        share.weather_area = inside;
        share.pervious_area = inside * r.area[sc] * (1 - r.imperv_pct[sc]) * ft2 / full;
        share.impervious_area = inside * r.area[sc] * r.imperv_pct[sc] * ft2 / full;
        share.lid_area = inside * lids / full;
        preview.shares.push_back(share);
        preview.mesh_weather_area[sc] -= inside;
    }
    error =
        m.stage.initialize(mesh, router.state(), options, router.subsurface(), m.sources, preview);
    if (error.empty())
        error = e.enableSpatialSourceCheckpointing(m.stage);
    if (!error.empty())
        throw std::runtime_error(error);
}
struct SpatialBridgeWire {
    uint32_t version = 2;
    uint64_t count = 0, digest = 0, last_begin = 0;
    double end = 0, last_start = 0;
    std::vector<double> clocks;
    std::vector<runoff::QueuedSourceDelivery> pending;
    std::vector<runoff::SourceNodeDelivery> received, delivered, last;
    /**
     * @brief Size a test bridge-wire view from node receipts and source clock groups.
     *
     * Owns clock/receipt/delivery/last vectors sized for the intended archive, initially
     * default-valued. Does not capture live bridge values or decode an archive; io fills them later
     * using fixed-size expectations.
     *
     * @param[in] b Borrowed live bridge; only received().size() is sampled, with no pointer retained.
     * @param[in] groups Expected source-group clock count used to preallocate fixed clock storage.
     */
    explicit SpatialBridgeWire(const runoff::SourceWaterDeliveryBridge& b, std::size_t groups)
        : clocks(groups), received(b.received().size()), delivered(received), last(received) {}
    /**
     * @brief Transfer the test spatial-delivery bridge wire fields in fixed archive order.
     *
     * Transfers version/count/digest/end, fixed clocks, pending ordinal/availability/water records,
     * fixed received/delivered arrays, last begin/start and fixed last runoff/drain/rate rows. Reads
     * replace owned values; writes append current native bridge quantities. Existing fixed vector sizes
     * must match the checkpoint layout. Does not finish/check CRC or validate physical semantics;
     * decode errors propagate.
     *
     * @param[in,out] a Caller-owned read/write archive advanced through metadata, clocks, queued
     *     deliveries and node receipt vectors.
     */
    void io(detail::CompletedSourceArchive& a) {
        a(version, count, digest, end);
        a.fixed(clocks);
        a.sequence(pending, [&](auto& q) {
            auto& d = q.water;
            a(q.ordinal, q.available, d.source, d.node, d.start, d.end, d.volume, d.drain);
        });
        a.sequence(received, [&](auto& d) { a(d.runoff, d.drain); }, true);
        a.sequence(delivered, [&](auto& d) { a(d.runoff, d.drain); }, true);
        a(last_begin, last_start);
        a.sequence(last, [&](auto& d) { a(d.runoff, d.drain, d.rate); }, true);
    }
};
}
TEST(EngineCheckpoint, SpatialReceiptsPreserveVolumesAtEveryClosedReceiverStepAndCadence) {
    for (int lid = 0; lid < 3; ++lid)
        for (double cadence : {.7, .31, 1.}) {
            const auto name =
                "spatial-closed-" + std::to_string(lid) + "-" + std::to_string(cadence);
            SCOPED_TRACE(name);
            SurfaceSession m;
            ASSERT_NO_THROW(startClosedSpatial(m, name, lid, cadence));
            const auto& c = m.engine.context();
            const double initial = c.mass_balance.routing_init_storage;
            const auto initial_inflow = c.nodes.stat_total_inflow_vol;
            double previous = 0;
            int steps = 0;
            bool pending = false;
            while (c.elapsed_ms < c.options.totalDurationMs()) {
                ASSERT_NO_THROW(step(m.engine));
                const auto* b = m.engine.sourceDelivery();
                double sink = 0, outfall = 0, total = 0, queued = 0;
                for (int n = 0; n < c.n_nodes(); ++n) {
                    const double v = b->delivered()[n].runoff + b->delivered()[n].drain;
                    total += v;
                    if (!lidnode::active(c, n))
                        EXPECT_NEAR(c.nodes.stat_total_inflow_vol[n] - initial_inflow[n], v, 1e-9);
                    EXPECT_NEAR(c.nodes.stat_lat_inflow_vol[n], v, 1e-9);
                    if (c.nodes.type[n] == NodeType::OUTFALL) {
                        outfall += v;
                        EXPECT_NEAR(c.nodes.stat_total_outflow_vol[n], v, 1e-9);
                    } else
                        sink += v;
                }
                for (const auto& q : b->pending())
                    queued += q.water.volume;
                pending |= queued > 0;
                EXPECT_NEAR(c.mass_balance.routing_final_storage - initial, sink, 1e-9);
                EXPECT_NEAR(c.mass_balance.routing_outflow, outfall, 1e-9);
                EXPECT_NEAR(c.mass_balance.routing_wet_weather, total, 1e-9);
                EXPECT_NEAR(c.mass_balance.routing_report[1], total, 1e-9);
                EXPECT_DOUBLE_EQ(c.mass_balance.routing_external, 0);
                EXPECT_NEAR(c.mass_balance.runoff_runoff + c.mass_balance.runoff_lid_drain,
                            total + queued, 1e-9);
                EXPECT_NEAR(m.sources.balanceResidual(0), 0, 1e-8);
                EXPECT_GE(total, previous);
                previous = total;
                if (++steps == 11) {
                    auto path = root() / (name + ".chk");
                    ASSERT_EQ(m.engine.saveCheckpoint(path.string()), "");
                    SurfaceSession restored;
                    ASSERT_NO_THROW(startClosedSpatial(restored, name + "-restored", lid, cadence));
                    ASSERT_EQ(restored.engine.loadCheckpoint(path.string()), "");
                    compare(m.engine, restored.engine, name + "-opening");
                    for (int k = 0; k < 3; ++k) {
                        step(m.engine);
                        step(restored.engine);
                    }
                    compare(m.engine, restored.engine, name + "-continued");
                }
            }
            EXPECT_TRUE(pending);
            EXPECT_GT(previous, 0);
            m.engine.end();
        }
}
TEST(EngineCheckpoint, SpatialBridgeCorruptionRejectsBeforeEveryComponentInstallation) {
    SurfaceSession a;
    ASSERT_NO_THROW(a.start("spatial-atomic-source", 2, 2, 1, 0, true, true));
    while (a.engine.context().current_time < 16 || a.engine.sourceDelivery()->pending().empty())
        ASSERT_NO_THROW(step(a.engine));
    const auto good = root() / "spatial-atomic.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    detail::CompletedSourceArchive input(bytes(good));
    std::string magic, model, payload;
    uint32_t version, sum;
    input(magic, version, model, sum, payload);
    input.finish();
    ASSERT_EQ(version, 105);
    auto bridge = *a.engine.sourceDelivery();
    detail::CompletedSourceArchive saved;
    bridge.checkpoint(saved, false, a.sources);
    auto water = saved.bytes();
    detail::CompletedSourceArchive encoded;
    encoded(water);
    const auto location = payload.find(encoded.bytes());
    ASSERT_NE(location, std::string::npos);
    SpatialBridgeWire original(bridge, a.sources.clocks().groups().size());
    detail::CompletedSourceArchive read(water);
    original.io(read);
    read.finish();
    ASSERT_FALSE(original.pending.empty());
    std::array<double, 6> totals{};
    for (int sc = 0; sc < a.engine.context().n_subcatches(); ++sc)
        if (bridge.ownsSubcatch(sc)) {
            const auto& b = a.sources.ledgers()[sc];
            totals[0] += b.rain;
            totals[1] += b.evaporation;
            totals[2] += b.infiltration;
            totals[3] += b.aquifer_return;
        }
    for (const auto& b : bridge.received()) {
        totals[4] += b.runoff;
        totals[5] += b.drain;
    }
    detail::CompletedSourceArchive stored_totals;
    stored_totals(totals);
    const auto totals_at = payload.find(stored_totals.bytes());
    ASSERT_NE(totals_at, std::string::npos);
    ASSERT_EQ(payload.find(stored_totals.bytes(), totals_at + 1), std::string::npos);
    for (int mutation = 0; mutation < 22; ++mutation) {
        SCOPED_TRACE(mutation);
        auto w = original;
        switch (mutation) {
        case 0:
            w.version = 1;
            break;
        case 1:
            ++w.count;
            break;
        case 2:
            w.digest ^= 1;
            break;
        case 3:
            w.end += .1;
            break;
        case 4:
            w.clocks[0] += 1;
            break;
        case 5:
            w.pending[0].available += 1;
            break;
        case 6:
            w.pending[0].water.volume += 1;
            break;
        case 7:
            w.pending[0].water.node = 99;
            break;
        case 8:
            w.delivered[0].runoff += 1;
            break;
        case 9:
            w.last_start = w.end;
            break;
        case 10:
            w.last_begin = w.count + 1;
            break;
        case 11:
            w.last[0].rate += 1;
            break;
        case 12:
            w.last[0].drain = std::numeric_limits<double>::quiet_NaN();
            break;
        }
        detail::CompletedSourceArchive changed;
        w.io(changed);
        auto badwater = changed.bytes();
        detail::CompletedSourceArchive newfield;
        newfield(badwater);
        auto bad = payload;
        bad.replace(location, encoded.bytes().size(), newfield.bytes());
        if (mutation >= 13 && mutation < 19) {
            auto inconsistent = totals;
            inconsistent[mutation - 13] += .1;
            detail::CompletedSourceArchive fields;
            fields(inconsistent);
            bad.replace(totals_at, stored_totals.bytes().size(), fields.bytes());
        }
        auto checksum = crc(bad);
        auto format = mutation == 19 ? uint32_t(4) : version;
        detail::CompletedSourceArchive out;
        out(magic, format, model, checksum, bad);
        auto invalid = out.bytes();
        if (mutation == 20)
            invalid.pop_back();
        if (mutation == 21)
            invalid.push_back('x');
        auto path = root() / ("spatial-bad-" + std::to_string(mutation) + ".chk");
        write(path, invalid);
        SurfaceSession target;
        ASSERT_NO_THROW(target.start("spatial-atomic-target-" + std::to_string(mutation), 2, 2, 1,
                                     0, true, true));
        const auto before = root() / "spatial-target-before.chk";
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()), "");
        auto fresh = bytes(before);
        EXPECT_FALSE(target.engine.loadCheckpoint(path.string()).empty());
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), fresh);
        ASSERT_EQ(target.engine.loadCheckpoint(good.string()), "");
        compare(a.engine, target.engine, "spatial-valid-retry-" + std::to_string(mutation));
    }
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, SpatialSourceReturnPreservesLowStorageAtShortCadence) {
    for (bool all : {false, true}) {
        SCOPED_TRACE(all);
        SurfaceSession m;
        ASSERT_NO_THROW(
            m.start("spatial-low-storage-" + std::to_string(all), 3, 1, 1, 6, true, true, all));
        const auto end = all ? m.engine.context().options.totalDurationMs() / 1000 : 100;
        while (m.engine.context().elapsed_ms < end * 1000)
            ASSERT_NO_THROW(step(m.engine));
        for (int group = 0; group < int(m.sources.clocks().groups().size()); ++group)
            EXPECT_NEAR(m.sources.balanceResidual(group), 0, 1e-8);
        if (all) {
            EXPECT_EQ(m.sources.clocks().groups().size(), 2);
            EXPECT_DOUBLE_EQ(m.engine.context().subcatches.stat_precip_vol[2], 0);
        }
    }
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, OwnedOutfallReturnsPreserveEveryAcceptedVolumeAndRunoffBalance) {
    for (int lid = 0; lid < 3; ++lid)
        for (double cadence : {.31, .7, 1.}) {
            const auto name =
                "owned-outfall-closed-" + std::to_string(lid) + "-" + std::to_string(cadence);
            SCOPED_TRACE(name);
            const bool multiple = lid == 2 && cadence == .31;
            SurfaceSession a;
            ASSERT_NO_THROW(startClosedSpatial(a, name, lid, cadence, true, multiple));
            int steps = 0;
            bool applied = false;
            while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
                ASSERT_NO_THROW(step(a.engine));
                const auto& c = a.engine.context();
                const auto* b = a.engine.sourceDelivery();
                double routed = 0;
                for (int n = 0; n < c.n_nodes(); ++n)
                    if (c.nodes.type[n] == NodeType::OUTFALL)
                        routed += b->delivered()[n].runoff + b->delivered()[n].drain;
                EXPECT_NEAR(a.sources.ledgers()[0].outfall_return, routed, 1e-10);
                EXPECT_NEAR(c.mass_balance.routing_outflow, routed, 1e-10);
                EXPECT_NEAR(c.mass_balance.runoff_runon, routed, 1e-10);
                EXPECT_DOUBLE_EQ(c.subcatches.outfall_runon_vol[0], 0);
                EXPECT_DOUBLE_EQ(a.sources.outfallReturnEnd(), c.current_time);
                EXPECT_NEAR(a.sources.balanceResidual(0), 0, 1e-8);
                const auto& mb = c.mass_balance;
                EXPECT_NEAR(mb.runoff_init_store + mb.runoff_rainfall + mb.runoff_runon -
                                mb.runoff_evap - mb.runoff_infil - mb.runoff_runoff -
                                mb.runoff_lid_drain - mb.runoff_final_store,
                            0, 1e-8);
                applied |= a.sources.ledgers()[0].runon > 0;
                if (++steps == 11) {
                    auto file = root() / (name + ".chk");
                    ASSERT_EQ(a.engine.saveCheckpoint(file.string()), "");
                    SurfaceSession restored;
                    ASSERT_NO_THROW(startClosedSpatial(restored, name + "-restored", lid, cadence,
                                                       true, multiple));
                    ASSERT_EQ(restored.engine.loadCheckpoint(file.string()), "");
                    compare(a.engine, restored.engine, name + "-restored");
                    for (int k = 0; k < 3; ++k) {
                        step(a.engine);
                        step(restored.engine);
                    }
                    compare(a.engine, restored.engine, name + "-continued");
                }
            }
            EXPECT_GT(a.sources.ledgers()[0].outfall_return, 0);
            EXPECT_TRUE(applied);
            if (multiple) {
                const int second = a.engine.context().node_names.find("DRAIN2");
                ASSERT_GE(second, 0);
                EXPECT_GT(a.engine.sourceDelivery()->delivered()[second].drain, 0);
            }
        }
}
TEST(EngineCheckpoint, OwnedOutfallReturnsContinueThroughBackwaterVariableStepsAndMultipleGraphs) {
    for (int lid = 0; lid < 4; ++lid)
        for (int receiver : {0, 2}) {
            const auto name =
                "owned-outfall-network-" + std::to_string(lid) + "-" + std::to_string(receiver);
            SCOPED_TRACE(name);
            SurfaceSession a, b;
            const bool all = receiver == 2;
            ASSERT_NO_THROW(a.start(name + "-a", lid, 0, 1, 6, true, true, all, receiver));
            ASSERT_NO_THROW(b.start(name + "-b", lid, 0, 1, 6, true, true, all, receiver));
            while (a.engine.context().current_time < 5)
                ASSERT_NO_THROW(step(a.engine));
            auto file = root() / (name + ".chk");
            ASSERT_EQ(a.engine.saveCheckpoint(file.string()), "");
            ASSERT_EQ(b.engine.loadCheckpoint(file.string()), "");
            compare(a.engine, b.engine, name + "-opening");
            int steps = 0;
            double min_dt = 1e9, max_dt = 0;
            while (a.engine.context().current_time < 35) {
                const auto before = a.engine.context().current_time;
                ASSERT_NO_THROW(step(a.engine));
                ASSERT_NO_THROW(step(b.engine));
                const auto& c = a.engine.context();
                const double dt = c.current_time - before;
                min_dt = std::min(min_dt, dt);
                max_dt = std::max(max_dt, dt);
                EXPECT_DOUBLE_EQ(c.subcatches.outfall_runon_vol[receiver], 0);
                EXPECT_NEAR(a.sources.ledgers()[receiver].outfall_return,
                            c.mass_balance.routing_outflow, 1e-9);
                double credits = 0;
                for (const auto& l : a.sources.ledgers())
                    credits += l.outfall_return + l.aquifer_return;
                EXPECT_NEAR(credits, c.mass_balance.runoff_runon, 1e-9);
                for (std::size_t g = 0; g < a.sources.clocks().groups().size(); ++g)
                    EXPECT_NEAR(a.sources.balanceResidual(int(g)), 0, 1e-7);
                if (++steps % 23 == 0)
                    compare(a.engine, b.engine, name + "-during");
            }
            EXPECT_LT(min_dt, max_dt);
            EXPECT_GT(a.sources.ledgers()[receiver].outfall_return, 0);
            if (lid == 3)
                EXPECT_GT(a.engine.context().mass_balance.routing_external, 0);
            compare(a.engine, b.engine, name + "-final");
        }
}
TEST(EngineCheckpoint,
     OwnedOutfallReceiptAttachmentBindsTopologyAndRefusesMappedDuplicateDischarge) {
    SurfaceSession a;
    ASSERT_NO_THROW(a.start("owned-outfall-topology", 2, 0, 0, 0, false, true, false, 0));
    a.engine.context().node_subtypes.outfalls.route_to[0] = 1;
    EXPECT_FALSE(a.engine.enableSpatialSourceCheckpointing(a.stage).empty());
    EXPECT_EQ(a.engine.sourceDelivery(), nullptr);
    a.engine.context().node_subtypes.outfalls.route_to[0] = 0;
    ASSERT_EQ(a.engine.enableSpatialSourceCheckpointing(a.stage), "");
    SurfaceSession mapped;
    ASSERT_NO_THROW(mapped.start("owned-outfall-mapped", 2, 3, 0, 0, false, true, false, 0));
    EXPECT_NE(mapped.engine.enableSpatialSourceCheckpointing(mapped.stage).find("mapped outfall"),
              std::string::npos);
    EXPECT_EQ(mapped.engine.sourceDelivery(), nullptr);
}
TEST(EngineCheckpoint, OwnedOutfallReceiptCorruptionPreservesAllComponentsAndAllowsValidRetry) {
    SurfaceSession a;
    ASSERT_NO_THROW(startClosedSpatial(a, "owned-outfall-atomic", 2, .31, true));
    while (a.engine.context().current_time < 5)
        ASSERT_NO_THROW(step(a.engine));
    const auto good = root() / "owned-outfall-atomic.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    auto valid = bytes(good), surface = engineSurface(valid),
         surface_data = surfacePayload(surface);
    auto cpu = nested(surface_data, "OpenSWMM CPU surface/source boundary").second;
    detail::CompletedSourceArchive ci(cpu);
    std::string cpu_magic, cpu_data;
    uint32_t cpu_v, cpu_endian, cpu_long, cpu_sum;
    ci(cpu_magic, cpu_v, cpu_endian, cpu_long, cpu_data, cpu_sum);
    ci.finish();
    auto common = nested(cpu_data, "OpenSWMM source boundary").second;
    detail::CompletedSourceArchive si(common);
    std::string source_magic, source_model, source_data;
    uint32_t source_v, source_endian, source_long, source_double, source_sum;
    si(source_magic, source_v, source_endian, source_long, source_double, source_model, source_data,
       source_sum);
    si.finish();
    auto end = a.sources.outfallReturnEnd();
    std::vector<double> volumes;
    for (const auto& l : a.sources.ledgers())
        volumes.push_back(l.outfall_return);
    auto queue = a.sources.pendingOutfallReturns();
    ASSERT_FALSE(queue.empty());
    const auto tuples = [](detail::CompletedSourceArchive& ar,
                           std::vector<runoff::SourceOutfallReturn>& rows) {
        ar.sequence(rows, [&](auto& q) { ar(q.source, q.available, q.volume); });
    };
    ASSERT_GT(volumes[0], 0);
    uint32_t profile = 1;
    detail::CompletedSourceArchive fields;
    fields(profile, end);
    for (auto v : volumes)
        fields(v);
    tuples(fields, queue);
    const auto at = source_data.find(fields.bytes());
    ASSERT_NE(at, std::string::npos);
    ASSERT_EQ(source_data.find(fields.bytes(), at + 1), std::string::npos);
    detail::CompletedSourceArchive ei(valid);
    std::string engine_magic, engine_model, engine_data;
    uint32_t engine_v, engine_sum;
    ei(engine_magic, engine_v, engine_model, engine_sum, engine_data);
    ei.finish();
    for (int mutation = 0; mutation < 13; ++mutation) {
        SCOPED_TRACE(mutation);
        auto changed_end = end;
        auto changed_volumes = volumes;
        auto changed_profile = profile;
        if (mutation == 0)
            changed_profile = 2;
        if (mutation == 1)
            changed_end += 1;
        if (mutation == 2)
            changed_end -= .1;
        if (mutation == 3)
            changed_volumes[0] = -1;
        if (mutation == 4)
            changed_volumes[0] = std::numeric_limits<double>::quiet_NaN();
        if (mutation == 5)
            changed_volumes[0] += .1;
        if (mutation == 6)
            changed_volumes[2] = .1; // unowned catchment
        auto changed_queue = queue;
        if (mutation == 7)
            changed_queue[0].available = end + 1;
        if (mutation == 8)
            changed_queue[0].available = 0;
        if (mutation == 9)
            changed_queue[0].source = 2;
        if (mutation == 10)
            changed_queue[0].volume = std::numeric_limits<double>::quiet_NaN();
        if (mutation == 11)
            changed_queue[0].volume = 0;
        if (mutation == 12)
            changed_queue[0].volume += 1;
        detail::CompletedSourceArchive replacement;
        replacement(changed_profile, changed_end);
        for (auto v : changed_volumes)
            replacement(v);
        tuples(replacement, changed_queue);
        auto changed_source = source_data;
        changed_source.replace(at, fields.bytes().size(), replacement.bytes());
        auto sum = crc(changed_source);
        detail::CompletedSourceArchive so;
        so(source_magic, source_v, source_endian, source_long, source_double, source_model,
           changed_source, sum);
        auto changed_cpu = replaceNested(cpu_data, source_magic, so.bytes());
        sum = crc(changed_cpu);
        detail::CompletedSourceArchive co;
        co(cpu_magic, cpu_v, cpu_endian, cpu_long, changed_cpu, sum);
        auto changed_surface =
            withSurfacePayload(surface, replaceNested(surface_data, cpu_magic, co.bytes()));
        auto changed_engine = replaceNested(
            engine_data, "OpenSWMM joint DW surface/source boundary", changed_surface);
        sum = crc(changed_engine);
        detail::CompletedSourceArchive eo;
        eo(engine_magic, engine_v, engine_model, sum, changed_engine);
        const auto bad = root() / ("owned-outfall-bad-" + std::to_string(mutation) + ".chk");
        write(bad, eo.bytes());
        SurfaceSession target;
        ASSERT_NO_THROW(startClosedSpatial(
            target, "owned-outfall-target-" + std::to_string(mutation), 2, .31, true));
        const auto before = root() / "owned-outfall-target.chk";
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()), "");
        auto fresh = bytes(before);
        EXPECT_FALSE(target.engine.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), fresh);
        ASSERT_EQ(target.engine.loadCheckpoint(good.string()), "");
        compare(a.engine, target.engine, "owned-outfall-retry-" + std::to_string(mutation));
    }
}

namespace {
/**
 * @brief Compare accepted source-history metadata, schemas and stored columns exactly.
 *
 * Fatally compares top-level metadata/table counts, then nonfatally compares fields/units/columns.
 * When a.sourceIntervals exists, dereferences b.sourceIntervals and compares those tables too;
 * compatible presence is required. Missing named tables throw via map::at even if counts match.
 * Does not mutate or serialize histories.
 *
 * @param[in] a Borrowed reference engine source history.
 * @param[in] b Borrowed candidate with matching optional detailed-history presence and named
 *     tables.
 */
void sameEngineHistory(const twoD::EngineSourceResultHistory& a,
                       const twoD::EngineSourceResultHistory& b) {
    ASSERT_EQ(a.sourceNames(), b.sourceNames());
    ASSERT_EQ(a.nodeNames(), b.nodeNames());
    ASSERT_EQ(a.outfallReturnsAvailable(), b.outfallReturnsAvailable());
    ASSERT_EQ(a.simulationStartDate(), b.simulationStartDate());
    ASSERT_EQ(a.detailedSourceIntervalsAvailable(), b.detailedSourceIntervalsAvailable());
    if (a.sourceIntervals()) {
        const auto& x = *a.sourceIntervals();
        const auto& y = *b.sourceIntervals();
        ASSERT_EQ(x.sourceNames(), y.sourceNames());
        ASSERT_EQ(x.pollutantNames(), y.pollutantNames());
        ASSERT_EQ(x.pollutantMassUnits(), y.pollutantMassUnits());
        ASSERT_EQ(x.outfallReturnsAvailable(), y.outfallReturnsAvailable());
        ASSERT_EQ(x.tables().size(), y.tables().size());
        for (const auto& [name, t] : x.tables()) {
            const auto& next = y.tables().at(name);
            EXPECT_EQ(t.fields, next.fields) << name;
            EXPECT_EQ(t.units, next.units) << name;
            EXPECT_EQ(t.columns, next.columns) << name;
        }
    }
    ASSERT_EQ(a.tables().size(), b.tables().size());
    for (const auto& [name, t] : a.tables()) {
        const auto& next = b.tables().at(name);
        EXPECT_EQ(t.fields, next.fields) << name;
        EXPECT_EQ(t.units, next.units) << name;
        EXPECT_EQ(t.columns, next.columns) << name;
    }
}
}
TEST(EngineCheckpoint, AcceptedSourceHistoryPreservesEveryEngineStepAndExactContinuation) {
    for (int lid = 0; lid < 3; ++lid)
        for (double cadence : {.31, .7}) {
            const auto name =
                "engine-history-" + std::to_string(lid) + "-" + std::to_string(cadence);
            SCOPED_TRACE(name);
            twoD::EngineSourceResultHistory history, resumed;
            SurfaceSession a, control, b;
            const bool multiple = lid == 2;
            ASSERT_NO_THROW(startClosedSpatial(a, name, lid, cadence, true, multiple));
            ASSERT_NO_THROW(
                startClosedSpatial(control, name + "-control", lid, cadence, true, multiple));
            ASSERT_EQ(a.engine.setEngineSourceResultHistory(history), "");
            EXPECT_DOUBLE_EQ(history.tables().at("boundaries").columns[2][0], 0);
            EXPECT_TRUE(history.outfallReturnsAvailable());
            bool repeated = false, leading = false, pending = false;
            int steps = 0;
            bool restored = false;
            while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
                ASSERT_NO_THROW({
                    step(a.engine);
                    step(control.engine);
                });
                ++steps;
                const auto& boundaries = history.tables().at("boundaries").columns;
                ASSERT_EQ(boundaries[0].size(), std::size_t(steps + 1));
                EXPECT_DOUBLE_EQ(boundaries[2].back(), a.engine.context().current_time);
                EXPECT_DOUBLE_EQ(boundaries[4].back(), a.stage.completedEnd());
                EXPECT_DOUBLE_EQ(boundaries[5].back(), a.sources.outfallReturnEnd());
                repeated |= boundaries[3].back() == boundaries[3][boundaries[3].size() - 2];
                leading |= boundaries[5].back() > boundaries[4].back();
                pending |= history.tables().at("pending_outfall_return").rows() > 0;
                compare(a.engine, control.engine, name + "-physics");
                if (restored) {
                    ASSERT_NO_THROW(step(b.engine));
                    sameEngineHistory(history, resumed);
                    compare(a.engine, b.engine, name + "-continued");
                }
                if (steps == 25) {
                    const auto checkpoint = root() / (name + ".chk");
                    ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
#ifdef OPENSWMM_HAS_HDF5_MODEL
                    const auto file = root() / (name + ".h5");
                    ASSERT_EQ(history.save(file.string()), "");
                    bool available = false;
                    ASSERT_EQ(
                        twoD::EngineSourceResultHistory::read(file.string(), resumed, available),
                        "");
                    ASSERT_TRUE(available);
                    sameEngineHistory(history, resumed);
                    twoD::SourceResultHistory component;
                    available = true;
                    EXPECT_EQ(twoD::SourceResultHistory::read(file.string(), component, available),
                              "");
                    EXPECT_FALSE(available);
#else
                    resumed = history;
#endif
                    ASSERT_NO_THROW(
                        startClosedSpatial(b, name + "-resumed", lid, cadence, true, multiple));
                    const auto original = history.tables().at("boundaries").columns;
                    EXPECT_FALSE(b.engine.resumeEngineSourceResultHistory(resumed).empty());
                    ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
                    ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed), "");
                    EXPECT_EQ(history.tables().at("boundaries").columns, original);
                    sameEngineHistory(history, resumed);
                    restored = true;
                }
            }
            EXPECT_TRUE(repeated);
            EXPECT_TRUE(leading);
            EXPECT_TRUE(pending);
            EXPECT_TRUE(restored);
            const auto& credits = history.tables().at("source_water").columns[16];
            EXPECT_GT(*std::max_element(credits.begin(), credits.end()), 0);
            const auto last = history.tables().at("boundaries").columns;
            double elapsed = 0;
            ASSERT_EQ(a.engine.step(&elapsed), 0);
            EXPECT_EQ(history.tables().at("boundaries").columns, last);
            ASSERT_EQ(a.engine.end(), 0);
#ifdef OPENSWMM_HAS_HDF5_MODEL
            EXPECT_EQ(history.save((root() / (name + "-final.h5")).string()), "");
#endif
        }
}
TEST(EngineCheckpoint, PairedSourceIntervalsPreserveBothClocksWaterAndRestoredContinuation) {
    for (int lid = 0; lid < 3; ++lid)
        for (double cadence : {.31, .7}) {
            const auto name =
                "paired-history-" + std::to_string(lid) + "-" + std::to_string(cadence);
            SCOPED_TRACE(name);
            twoD::EngineSourceResultHistory history, resumed;
            SurfaceSession a, control, b;
            const bool returns = lid != 0, multiple = lid == 2;
            ASSERT_NO_THROW(startClosedSpatial(a, name, lid, cadence, returns, multiple));
            ASSERT_NO_THROW(
                startClosedSpatial(control, name + "-control", lid, cadence, returns, multiple));
            ASSERT_EQ(a.engine.setEngineSourceResultHistory(history, true), "");
            ASSERT_TRUE(history.sourceIntervals());
            EXPECT_EQ(history.sourceIntervals()->tables().at("boundaries").rows(), 1u);
            twoD::SourceResultHistory separate;
            EXPECT_THROW(a.engine.surfaceRouter2D().setSourceResultHistory(separate),
                         std::invalid_argument);
            bool repeated = false, multiple_intervals = false, earlier_credits = false,
                 restored = false;
            int steps = 0;
            while (steps < 45 &&
                   a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
                ASSERT_NO_THROW({
                    step(a.engine);
                    step(control.engine);
                });
                ++steps;
                const auto& engine = history.tables().at("boundaries").columns;
                const auto& source = history.sourceIntervals()->tables().at("boundaries").columns;
                ASSERT_EQ(source[0].size(), std::size_t(a.stage.intervals() + 1));
                EXPECT_EQ(source[3].back(), a.stage.completedEnd());
                EXPECT_EQ(source[3].back(), engine[4].back());
                repeated |= engine[3].back() == engine[3][engine[3].size() - 2];
                multiple_intervals |= engine[3].back() - engine[3][engine[3].size() - 2] > 1;
                if (returns)
                    earlier_credits |=
                        history.sourceIntervals()->tables().at("outfall_clock").columns[1].back() <
                        engine[5].back();
                compare(a.engine, control.engine, name + "-physics");
                if (restored) {
                    ASSERT_NO_THROW(step(b.engine));
                    sameEngineHistory(history, resumed);
                    compare(a.engine, b.engine, name + "-continued");
                }
                if (steps == 25) {
                    const auto checkpoint = root() / (name + ".chk");
                    ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
#ifdef OPENSWMM_HAS_HDF5_MODEL
                    const auto file = root() / (name + ".h5");
                    ASSERT_EQ(history.save(file.string()), "");
                    bool available = false;
                    ASSERT_EQ(
                        twoD::EngineSourceResultHistory::read(file.string(), resumed, available),
                        "");
                    ASSERT_TRUE(available);
                    sameEngineHistory(history, resumed);
                    twoD::SourceResultHistory component;
                    available = true;
                    EXPECT_EQ(twoD::SourceResultHistory::read(file.string(), component, available),
                              "");
                    EXPECT_FALSE(available);
#else
                    resumed = history;
#endif
                    ASSERT_NO_THROW(
                        startClosedSpatial(b, name + "-resumed", lid, cadence, returns, multiple));
                    const auto prefix = history;
                    EXPECT_FALSE(b.engine.resumeEngineSourceResultHistory(resumed).empty());
                    ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
                    ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed), "");
                    sameEngineHistory(history, prefix);
                    sameEngineHistory(history, resumed);
                    compare(a.engine, b.engine, name + "-rebind");
                    restored = true;
                }
            }
            EXPECT_TRUE(repeated);
            EXPECT_TRUE(multiple_intervals);
            EXPECT_TRUE(restored);
            EXPECT_EQ(earlier_credits, returns);
            const auto prefix = history;
            ASSERT_EQ(a.engine.end(), 0);
            ASSERT_EQ(control.engine.end(), 0);
            sameEngineHistory(history, prefix);
            EXPECT_EQ(a.stage.intervals(), control.stage.intervals());
            EXPECT_EQ(a.stage.completedEnd(), control.stage.completedEnd());
            EXPECT_EQ(a.sources.ledgers()[0].outfall_return,
                      control.sources.ledgers()[0].outfall_return);
            EXPECT_EQ(a.engine.surfaceRouter2D().state().depth,
                      control.engine.surfaceRouter2D().state().depth);
#ifdef OPENSWMM_HAS_HDF5_MODEL
            EXPECT_EQ(history.save((root() / (name + "-final.h5")).string()), "");
#endif
        }
}
TEST(EngineCheckpoint,
     PairedSourceHistoryInspectsEarlierSnapshotsAndOwnershipWithoutRollingBackEngine) {
    SurfaceSession a;
    ASSERT_NO_THROW(startClosedSpatial(a, "paired-guards", 2, .31, true, true));
    twoD::EngineSourceResultHistory history;
    ASSERT_EQ(a.engine.setEngineSourceResultHistory(history, true), "");
    for (int k = 0; k < 25; ++k)
        ASSERT_NO_THROW(step(a.engine));
    ASSERT_GT(history.sourceIntervals()->tables().at("boundaries").rows(), 1u);
    const auto checkpoint = root() / "paired-guards.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
    for (int mutation = 0; mutation < 7; ++mutation) {
        SCOPED_TRACE(mutation);
        auto edited = history;
        auto& source = *const_cast<twoD::SourceResultHistory*>(edited.sourceIntervals());
        auto& t = const_cast<std::map<std::string, twoD::SourceResultTable>&>(source.tables());
        switch (mutation) {
        case 0:
            t.at("footprints").columns[6][0] += .001;
            break; // valid weather area, false static identity
        case 1:
            t.at("lid_usages").columns[4][0] += .001;
            break;
        case 2:
            t.at("receiver").columns[6].back() += .001;
            break; // pending ET invisible to engine water snapshot
        case 3:
            for (auto& c : t.at("boundaries").columns)
                c.pop_back();
            break;
        case 4:
            t.at("outfall_clock").columns[1].back() = a.sources.outfallReturnEnd();
            break;
        case 5:
            const_cast<std::vector<std::string>&>(source.sourceNames())[0] = "different";
            break;
        case 6:
            t.at("boundaries").columns[3].back() += .001;
            break;
        }
        const auto original = edited;
        SurfaceSession target;
        ASSERT_NO_THROW(startClosedSpatial(target, "paired-target-" + std::to_string(mutation), 2,
                                           .31, true, true));
        ASSERT_EQ(target.engine.loadCheckpoint(checkpoint.string()), "");
        const auto before = root() / ("paired-target-" + std::to_string(mutation) + ".chk");
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()), "");
        const auto saved = bytes(before);
        EXPECT_FALSE(target.engine.resumeEngineSourceResultHistory(edited).empty());
        sameEngineHistory(edited, original);
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), saved);
        auto valid = history;
        ASSERT_EQ(target.engine.resumeEngineSourceResultHistory(valid), "");
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), saved);
        ASSERT_NO_THROW(step(target.engine));
    }
}
TEST(EngineCheckpoint, PairedSourceHistorySupportsInitialRebindAndVectorRichardsBackwater) {
    for (bool returns : {false, true}) {
        const auto name = std::string("paired-vector-") + (returns ? "returns" : "ordinary");
        SurfaceSession a, b, control;
        twoD::EngineSourceResultHistory history, resumed;
        ASSERT_NO_THROW(a.start(name, 3, 0, 1, 6, true, true, true, returns ? 2 : -1));
        ASSERT_NO_THROW(
            control.start(name + "-control", 3, 0, 1, 6, true, true, true, returns ? 2 : -1));
        ASSERT_EQ(a.engine.setEngineSourceResultHistory(history, true), "");
        resumed = history;
        const auto initial = root() / (name + "-initial.chk");
        ASSERT_EQ(a.engine.saveCheckpoint(initial.string()), "");
        ASSERT_NO_THROW(b.start(name + "-resumed", 3, 0, 1, 6, true, true, true, returns ? 2 : -1));
        ASSERT_EQ(b.engine.loadCheckpoint(initial.string()), "");
        ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed), "");
        for (int k = 0; k < 20; ++k) {
            ASSERT_NO_THROW({
                step(a.engine);
                step(b.engine);
                step(control.engine);
            });
            sameEngineHistory(history, resumed);
            compare(a.engine, b.engine, name + "-continued");
            compare(a.engine, control.engine, name + "-physics");
        }
#ifdef OPENSWMM_HAS_HDF5_MODEL
        const auto h5 = root() / (name + ".h5");
        ASSERT_EQ(history.save(h5.string()), "");
        twoD::EngineSourceResultHistory read;
        bool available = false;
        ASSERT_EQ(twoD::EngineSourceResultHistory::read(h5.string(), read, available), "");
        ASSERT_TRUE(available);
        sameEngineHistory(history, read);
#endif
    }
}
TEST(EngineCheckpoint, AcceptedSourceHistoryRefusesWrongProfilesRestoreOrderAndFinalProjection) {
    twoD::EngineSourceResultHistory empty, history;
    SWMMEngine ordinary;
    ASSERT_NO_THROW(start(ordinary, "engine-history-ordinary", 0, true, 0, false, true));
    EXPECT_FALSE(ordinary.setEngineSourceResultHistory(empty).empty());
    EXPECT_TRUE(empty.tables().empty());
    SurfaceSession mesh;
    ASSERT_NO_THROW(mesh.start("engine-history-mesh"));
    EXPECT_FALSE(mesh.engine.setEngineSourceResultHistory(empty).empty());
    EXPECT_TRUE(empty.tables().empty());
    SurfaceSession a;
    ASSERT_NO_THROW(startClosedSpatial(a, "engine-history-guards", 2, .31, true));
    ASSERT_EQ(a.engine.setEngineSourceResultHistory(history), "");
    EXPECT_FALSE(a.engine.setEngineSourceResultHistory(empty).empty());
    for (int k = 0; k < 25; ++k)
        ASSERT_NO_THROW(step(a.engine));
    const auto good = root() / "engine-history-guards.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    twoD::EngineSourceResultHistory attached;
    SurfaceSession initial;
    ASSERT_NO_THROW(startClosedSpatial(initial, "engine-history-attached", 2, .31, true));
    ASSERT_EQ(initial.engine.setEngineSourceResultHistory(attached), "");
    const auto before = root() / "engine-history-attached.chk";
    ASSERT_EQ(initial.engine.saveCheckpoint(before.string()), "");
    auto original = bytes(before);
    EXPECT_FALSE(initial.engine.loadCheckpoint(good.string()).empty());
    ASSERT_EQ(initial.engine.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(before), original);
    for (int mutation = 0; mutation < 10; ++mutation) {
        SCOPED_TRACE(mutation);
        auto edited = history;
        auto& t = const_cast<std::map<std::string, twoD::SourceResultTable>&>(edited.tables());
        switch (mutation) {
        case 0:
            t.at("boundaries").columns[2].back() += .001;
            break;
        case 1:
            t.at("boundaries").columns[7].back() += 1;
            break; // semantic but false final projection
        case 2:
            for (auto& v : t.at("source_water").columns[3])
                v = .25;
            break; // valid fraction, false identity
        case 3:
            t.at("group_water").columns[3].back() += 1;
            break;
        case 4:
            t.at("node_delivery").columns[4].back() += 1;
            break;
        case 5:
            t.at("boundaries").units[2] = "h";
            break;
        case 6:
            const_cast<std::vector<std::string>&>(edited.sourceNames())[0] = "other catchment";
            break;
        case 7:
            t.at("boundaries").columns[3].back() += 1;
            break;
        case 8:
            t.at("source_water").columns[16].back() = -1;
            break;
        case 9:
            ASSERT_GT(t.at("pending_outfall_return").rows(), 0u);
            t.at("pending_outfall_return").columns[4].back() += 1;
            break;
        }
        SurfaceSession target;
        ASSERT_NO_THROW(startClosedSpatial(
            target, "engine-history-mutant-" + std::to_string(mutation), 2, .31, true));
        ASSERT_EQ(target.engine.loadCheckpoint(good.string()), "");
        const auto path = root() / ("engine-history-mutant-" + std::to_string(mutation) + ".chk");
        ASSERT_EQ(target.engine.saveCheckpoint(path.string()), "");
        const auto bytes_before = bytes(path);
        const auto columns = t.at("boundaries").columns;
        EXPECT_FALSE(target.engine.resumeEngineSourceResultHistory(edited).empty());
        EXPECT_EQ(t.at("boundaries").columns, columns);
        ASSERT_EQ(target.engine.saveCheckpoint(path.string()), "");
        EXPECT_EQ(bytes(path), bytes_before);
        auto valid = history;
        ASSERT_EQ(target.engine.resumeEngineSourceResultHistory(valid), "");
        ASSERT_NO_THROW(step(target.engine));
    }
}
TEST(EngineCheckpoint,
     AcceptedSourceHistoryKeepsDisabledReturnsUnavailableAndVectorRichardsBackwater) {
    for (bool returns : {false, true}) {
        twoD::EngineSourceResultHistory history, resumed;
        SurfaceSession a, b;
        const auto name =
            std::string("engine-history-vector-") + (returns ? "returns" : "ordinary");
        ASSERT_NO_THROW(a.start(name, 3, 0, 1, 6, true, true, true, returns ? 2 : -1));
        ASSERT_EQ(a.engine.setEngineSourceResultHistory(history), "");
        EXPECT_EQ(history.outfallReturnsAvailable(), returns);
        EXPECT_EQ(history.tables().count("pending_outfall_return"), returns ? 1u : 0u);
        for (int k = 0; k < 16; ++k)
            ASSERT_NO_THROW(step(a.engine));
        const auto file = root() / (name + ".chk");
        ASSERT_EQ(a.engine.saveCheckpoint(file.string()), "");
        resumed = history;
        ASSERT_NO_THROW(b.start(name + "-resumed", 3, 0, 1, 6, true, true, true, returns ? 2 : -1));
        ASSERT_EQ(b.engine.loadCheckpoint(file.string()), "");
        ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed), "");
        for (int k = 0; k < 4; ++k) {
            ASSERT_NO_THROW({
                step(a.engine);
                step(b.engine);
            });
            sameEngineHistory(history, resumed);
            compare(a.engine, b.engine, name);
        }
#ifdef OPENSWMM_HAS_HDF5_MODEL
        const auto h5 = root() / (name + ".h5");
        ASSERT_EQ(history.save(h5.string()), "");
        twoD::EngineSourceResultHistory read;
        bool available = false;
        ASSERT_EQ(twoD::EngineSourceResultHistory::read(h5.string(), read, available), "");
        ASSERT_TRUE(available);
        sameEngineHistory(history, read);
#endif
    }
}
#ifdef OPENSWMM_HAS_HDF5_MODEL
TEST(EngineCheckpoint,
     AcceptedSourceHistoryPublicationAndMalformedReaderPreservePreviousArtifacts) {
    twoD::EngineSourceResultHistory history;
    SurfaceSession a;
    ASSERT_NO_THROW(startClosedSpatial(a, "engine-history-files", 0, .7, true));
    ASSERT_EQ(a.engine.setEngineSourceResultHistory(history), "");
    for (int k = 0; k < 12; ++k)
        ASSERT_NO_THROW(step(a.engine));
    const auto good = root() / "engine-history-files.h5";
    ASSERT_EQ(history.save(good.string()), "");
    const auto original = bytes(good);
    auto invalid = history;
    const_cast<std::map<std::string, twoD::SourceResultTable>&>(invalid.tables())
        .at("boundaries")
        .columns[5]
        .back() = 0;
    EXPECT_FALSE(invalid.save(good.string()).empty());
    EXPECT_EQ(bytes(good), original);
    auto unrelated = root() / "engine-history-unrelated.h5";
    write(unrelated, "keep unrelated data");
    EXPECT_FALSE(history.save(unrelated.string()).empty());
    EXPECT_EQ(bytes(unrelated), "keep unrelated data");
    const auto broken = root() / "engine-history-broken.h5";
    write(broken, original.substr(0, original.size() / 2));
    twoD::EngineSourceResultHistory out = history;
    bool available = true;
    EXPECT_FALSE(twoD::EngineSourceResultHistory::read(broken.string(), out, available).empty());
    EXPECT_FALSE(available);
    sameEngineHistory(history, out);
    available = true;
    EXPECT_FALSE(twoD::EngineSourceResultHistory::read(
                     (root() / "missing-engine-history.h5").string(), out, available)
                     .empty());
    EXPECT_FALSE(available);
    sameEngineHistory(history, out);
    ASSERT_EQ(twoD::EngineSourceResultHistory::read(good.string(), out, available), "");
    ASSERT_TRUE(available);
    sameEngineHistory(history, out);
}
#endif
#endif

namespace {
/**
 * @brief Author and start the saved-binary-output continuation fixture.
 *
 * Creates a temporary runoff fixture with Green-Ampt/intensity rain, reopens its deck on e,
 * initializes, prepares requested output capability and starts with save_results=1. Uses one thread
 * for reproducible artifacts. Temporary close and deck writes are unchecked; rejected
 * lifecycle/capability operations throw with partial e state retained. Caller owns cleanup; metric
 * selects another numeric model rather than an equivalent-unit conversion.
 *
 * @param[in,out] e Caller-owned openable engine, left running with saved results on success.
 * @param[in] name Raw root-relative artifact stem; existing .out is removed and deck/report may be
 *     overwritten.
 * @param[in] lid NODE LID mode forwarded to the runoff fixture.
 * @param[in] average Enable averaged report values when true.
 * @param[in] late Delay report_start by thirteen seconds when true.
 * @param[in] subset 0 all default report rows, 1 only storage S among node/link rows with subcatch
 *     defaults retained, other nonzero disables all node/link/subcatch rows.
 * @param[in] metric Change FLOW_UNITS to CMS without transcribing remaining authored numeric
 *     values.
 * @param[in] qualify Prepare binary-output checkpointing before start and enable the session
 *     afterward when true.
 */
void startBinary(SWMMEngine& e, const std::string& name, int lid = 0, bool average = false,
                 bool late = false, int subset = 0, bool metric = false, bool qualify = true) {
    SWMMEngine authored;
    startRunoff(authored, name, 2, 0, lid, false);
    authored.close();
    const auto path = root() / name;
    std::filesystem::remove(path.string() + ".out");
    if (metric) {
        auto deck = bytes(path.string() + ".inp");
        deck.replace(deck.find("FLOW_UNITS CFS"), 14, "FLOW_UNITS CMS");
        write(path.string() + ".inp", deck);
    }
    if (e.open((path.string() + ".inp").c_str(), (path.string() + ".rpt").c_str(),
               (path.string() + ".out").c_str()))
        throw std::runtime_error(e.context().error_message);
    auto& ctx = e.context();
    ctx.options.num_threads = 1;
    ctx.options.rpt_averages = average;
    if (late)
        ctx.options.report_start = datetime::addSeconds(ctx.options.start_date, 13);
    if (subset) {
        std::fill(ctx.links.rpt_flag.begin(), ctx.links.rpt_flag.end(), 0);
        std::fill(ctx.nodes.rpt_flag.begin(), ctx.nodes.rpt_flag.end(), 0);
        if (subset == 1)
            ctx.nodes.rpt_flag[ctx.node_names.find("S")] = 1;
        else
            std::fill(ctx.subcatches.rpt_flag.begin(), ctx.subcatches.rpt_flag.end(), 0);
    }
    if (e.initialize())
        throw std::runtime_error(ctx.error_message);
    if (qualify) {
        auto error = e.prepareOutputCheckpointing();
        if (!error.empty())
            throw std::runtime_error(error);
    }
    if (e.start(1))
        throw std::runtime_error(ctx.error_message);
    if (qualify) {
        auto error = e.enableCheckpointing();
        if (!error.empty())
            throw std::runtime_error(error);
    }
}
/**
 * @brief Advance the started fixture to its total duration and end saved output.
 *
 * Uses step helper and throws on step/end failure. Requires successful steps to progress to a
 * reachable total duration; no iteration guard is applied. Does not report/close/destroy or remove
 * the output file.
 *
 * @param[in,out] e Caller-owned running engine whose elapsed clock is advanced until
 *     totalDurationMs.
 */
void finishBinary(SWMMEngine& e) {
    while (e.context().elapsed_ms < e.context().options.totalDurationMs())
        step(e);
    if (e.end())
        throw std::runtime_error(e.context().error_message);
}
/**
 * @brief Compare binary artifact bytes and expected report-period count.
 *
 * Nonfatally compares bytes, then fatally opens the first with OutputReader and checks its period
 * count. Requires complete/readable artifacts; unchecked byte reads can both yield empty strings,
 * so the separate reader-open gate matters. Reader owns its cleanup and no file is modified.
 *
 * @param[in] a Raw first root-relative artifact stem; .out appended.
 * @param[in] b Raw comparison artifact stem; .out appended.
 * @param[in] periods Expected period count in the first output artifact.
 */
void sameBinary(const std::string& a, const std::string& b, int periods) {
    const auto pa = root() / (a + ".out"), pb = root() / (b + ".out");
    EXPECT_EQ(bytes(pa), bytes(pb));
    OutputReader reader;
    ASSERT_TRUE(reader.open(pa.string().c_str()));
    EXPECT_EQ(reader.period_count(), periods);
}
// The output component is the final length-prefixed field of formats 6/7/8.
// Read its explicit layout, then rehash the outer engine record for mutants.
/**
 * @brief Forge one binary-output checkpoint contract inconsistency and rehash its engine envelope.
 *
 * Requires valid native seven-int binary header, period count/layout and enough prefix bytes for
 * selected mutation. Checks suffix marker/span but does not harden every numeric offset against
 * arbitrary corrupt input. Decode/layout/allocation failures throw; no file is written and caller
 * input is unchanged.
 *
 * @param[in] checkpoint Borrowed complete known-layout engine archive with the binary-output record
 *     as its final length-prefixed field.
 * @param[in] mutation 0 output format, 1 period count, 2 prefix magic byte, 3 truncate prefix, 4
 *     append byte, 5 first period date +1 day, 6 first result NaN, 7 units header, 8 outer version
 *     decrement; other values only re-encode.
 * @return Owned changed engine archive bytes with its payload CRC recomputed.
 */
std::string changeBinary(const std::string& checkpoint, int mutation) {
    detail::CompletedSourceArchive in(checkpoint);
    std::string magic, model, payload;
    uint32_t version, sum;
    in(magic, version, model, sum, payload);
    in.finish();
    const int marker = 516114522;
    const std::string binary(reinterpret_cast<const char*>(&marker), sizeof(marker));
    const auto body =
        payload.rfind(binary); // last match is the native prefix header (live files have no footer)
    if (body == std::string::npos || body < 24)
        throw std::runtime_error("missing output checkpoint prefix");
    const auto offset = body - 24;
    uint64_t length = 0;
    std::memcpy(&length, payload.data() + offset, 8);
    if (offset + 8 + length != payload.size())
        throw std::runtime_error("invalid output checkpoint suffix");
    detail::CompletedSourceArchive wire(payload.substr(offset + 8));
    uint32_t format;
    int count;
    std::string prefix;
    wire(format, count, prefix);
    wire.finish();
    std::array<int, 7> native;
    std::memcpy(native.data(), prefix.data(), sizeof(native));
    const std::size_t row = 8 + 4 * (native[3] * (8 + native[6]) + native[4] * (6 + native[6]) +
                                     native[5] * (5 + native[6]) + 15);
    const auto header = prefix.size() - std::size_t(count) * row;
    if (mutation == 0)
        format = 2;
    if (mutation == 1)
        ++count;
    if (mutation == 2)
        prefix[0] ^= 1;
    if (mutation == 3)
        prefix.pop_back();
    if (mutation == 4)
        prefix.push_back('x');
    if (mutation == 5) {
        double date;
        std::memcpy(&date, prefix.data() + header, 8);
        date += 1;
        std::memcpy(prefix.data() + header, &date, 8);
    }
    if (mutation == 6) {
        const float value = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(prefix.data() + header + 8, &value, 4);
    }
    if (mutation == 7) {
        int bad = 123;
        std::memcpy(prefix.data() + 8, &bad, 4);
    }
    if (mutation == 8)
        --version;
    detail::CompletedSourceArchive changed;
    changed(format, count, prefix);
    auto output = changed.bytes();
    detail::CompletedSourceArchive suffix;
    suffix(output);
    payload.replace(offset, payload.size() - offset, suffix.bytes());
    sum = crc(payload);
    detail::CompletedSourceArchive out;
    out(magic, version, model, sum, payload);
    return out.bytes();
}
}

TEST(EngineCheckpoint, BinaryOutputContinuationPreservesReportGridAveragesSubsetsAndUnits) {
    for (int lid : {0, 1, 2, 3})
        for (bool average : {false, true})
            for (bool late : {false, true}) {
                const auto name = "binary-" + std::to_string(lid) + "-" + std::to_string(average) +
                                  "-" + std::to_string(late);
                SCOPED_TRACE(name);
                SWMMEngine a, b, control;
                const int subset = lid % 3;
                const bool metric = lid == 0;
                ASSERT_NO_THROW(startBinary(a, name, lid, average, late, subset, metric));
                ASSERT_NO_THROW(startBinary(control, name + "-control", lid, average, late, subset,
                                            metric, false));
                const double cut = lid == 0 ? 0 : (lid == 1 ? 1.4 : (lid == 2 ? 3. : 17.5));
                while (a.context().current_time < cut)
                    ASSERT_NO_THROW(step(a));
                const auto checkpoint = root() / (name + ".chk");
                ASSERT_EQ(a.saveCheckpoint(checkpoint.string()), "");
                ASSERT_NO_THROW(
                    startBinary(b, name + "-resumed", lid, average, late, subset, metric));
                ASSERT_EQ(b.loadCheckpoint(checkpoint.string()), "");
                compare(a, b, name + "-restored");
                OutputReader live;
                ASSERT_TRUE(live.openLive((root() / (name + "-resumed.out")).string().c_str()));
                EXPECT_EQ(live.period_count(),
                          std::max(0, int(a.context().next_report_ms / 3000) - 1 - (late ? 4 : 0)));
                ASSERT_NO_THROW(finishBinary(a));
                ASSERT_NO_THROW(finishBinary(b));
                ASSERT_NO_THROW(finishBinary(control));
                sameBinary(name, name + "-resumed", late ? 76 : 80);
                sameBinary(name, name + "-control", late ? 76 : 80);
            }
}
TEST(EngineCheckpoint, BinaryOutputMutantsPreserveFreshWriterAndAllowMatchingRetry) {
    SWMMEngine a;
    ASSERT_NO_THROW(startBinary(a, "binary-mutant-reference"));
    while (a.context().current_time < 18.1)
        ASSERT_NO_THROW(step(a));
    const auto good = root() / "binary-mutant-reference.chk";
    ASSERT_EQ(a.saveCheckpoint(good.string()), "");
    for (int mutation = 0; mutation < 9; ++mutation) {
        SCOPED_TRACE(mutation);
        const auto name = "binary-mutant-" + std::to_string(mutation);
        const auto bad = root() / (name + ".chk");
        ASSERT_NO_THROW(write(bad, changeBinary(bytes(good), mutation)));
        SWMMEngine b;
        ASSERT_NO_THROW(startBinary(b, name));
        const auto before = root() / (name + "-before.chk");
        ASSERT_EQ(b.saveCheckpoint(before.string()), "");
        const auto saved = bytes(before), header = bytes(root() / (name + ".out"));
        EXPECT_FALSE(b.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(b.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), saved);
        EXPECT_EQ(bytes(root() / (name + ".out")), header);
        ASSERT_EQ(b.loadCheckpoint(good.string()), "");
        compare(a, b, name + "-retry");
        ASSERT_EQ(b.end(), 0);
    }
    ASSERT_EQ(a.end(), 0);
}
TEST(EngineCheckpoint, BinaryOutputRestorePublicationFailureLeavesEngineUnchanged) {
    SWMMEngine a, b;
    ASSERT_NO_THROW(startBinary(a, "binary-publication-a"));
    ASSERT_NO_THROW(startBinary(b, "binary-publication-b"));
    while (a.context().current_time < 12.1)
        ASSERT_NO_THROW(step(a));
    const auto good = root() / "binary-publication.chk",
               before = root() / "binary-publication-before.chk",
               output = root() / "binary-publication-b.out";
    ASSERT_EQ(a.saveCheckpoint(good.string()), "");
    ASSERT_EQ(b.saveCheckpoint(before.string()), "");
    const auto saved = bytes(before), header = bytes(output);
    std::filesystem::permissions(output, std::filesystem::perms::owner_read);
    EXPECT_FALSE(b.loadCheckpoint(good.string()).empty());
    ASSERT_EQ(b.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(before), saved);
    EXPECT_EQ(bytes(output), header);
    std::filesystem::permissions(output, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_write);
    ASSERT_EQ(b.loadCheckpoint(good.string()), "");
    compare(a, b, "binary-publication-retry");
    ASSERT_EQ(a.end(), 0);
    ASSERT_EQ(b.end(), 0);
}
TEST(EngineCheckpoint, BinaryOutputModeRefusesExistingDestinationAndWrongLifecycle) {
    SWMMEngine authored;
    ASSERT_NO_THROW(start(authored, "binary-guard-model", 0, 0, false, false));
    ASSERT_EQ(authored.close(), 0);
    const auto inp = root() / "binary-guard-model.inp", out = root() / "binary-guard.out";
    write(out, "saved results");
    SWMMEngine e;
    ASSERT_EQ(e.open(inp.string().c_str(), nullptr, out.string().c_str()), 0);
    EXPECT_FALSE(e.prepareOutputCheckpointing().empty());
    ASSERT_EQ(e.initialize(), 0);
    EXPECT_FALSE(e.prepareOutputCheckpointing().empty());
    EXPECT_EQ(bytes(out), "saved results");
    std::filesystem::remove(out);
    auto* extra = new HeldOutput;
    e.plugin_factory().add_output_plugin(extra);
    EXPECT_FALSE(e.prepareOutputCheckpointing().empty());
    e.plugin_factory().unload_all();
    ASSERT_EQ(e.close(), 0);
    SWMMEngine fresh;
    ASSERT_EQ(fresh.open(inp.string().c_str(), nullptr, out.string().c_str()), 0);
    ASSERT_EQ(fresh.initialize(), 0);
#ifdef OPENSWMM_HAS_2D
    fresh.surfaceRouter2D().options().output_file = "unqualified-mesh.h5";
    EXPECT_FALSE(fresh.prepareOutputCheckpointing().empty());
    fresh.surfaceRouter2D().options().output_file = "";
#endif
    ASSERT_EQ(fresh.prepareOutputCheckpointing(), "");
    // An intervening creator is also protected by the exclusive open in start.
    write(out, "new results");
    EXPECT_NE(fresh.start(1), 0);
    EXPECT_EQ(bytes(out), "new results");
    ASSERT_EQ(fresh.close(), 0);
    SWMMEngine ordinary;
    ASSERT_NO_THROW(startBinary(ordinary, "binary-unprepared", 0, false, false, 0, false, false));
    EXPECT_FALSE(ordinary.prepareOutputCheckpointing().empty());
    EXPECT_FALSE(ordinary.enableCheckpointing().empty());
    ASSERT_EQ(ordinary.end(), 0);
    SWMMEngine no_output;
    ASSERT_NO_THROW(start(no_output, "binary-no-output"));
    EXPECT_FALSE(no_output.prepareOutputCheckpointing().empty());
}

TEST(EngineCheckpoint, OutputPublicationKeepsLiveStreamsAndRejectsReplacedFileIdentity) {
    const auto destination = root() / "publication-live", stage = root() / "publication-live-stage",
               moved = root() / "publication-live-moved";
    for (const auto& path : {destination, stage, moved})
        std::filesystem::remove(path);
    const auto open = [](const auto& path, bool existing = false) {
        return std::unique_ptr<FILE, decltype(&std::fclose)>(
            io::checkpointOpenOutput(path, existing), &std::fclose);
    };
    EXPECT_FALSE(open(stage, true));
    EXPECT_FALSE(std::filesystem::exists(stage));
    auto original = open(destination);
    ASSERT_TRUE(original);
    ASSERT_EQ(std::fwrite("header", 1, 6, original.get()), 6u);
    ASSERT_EQ(std::fflush(original.get()), 0);
    // File-backed restoration copies the prefix, then reopens its live stream.
    write(stage, "prefix");
    auto prepared = open(stage, true);
    ASSERT_TRUE(prepared);
    EXPECT_EQ(bytes(stage), "prefix");
    ASSERT_EQ(std::fseek(prepared.get(), 0, SEEK_END), 0);
    EXPECT_FALSE(open(destination));
    EXPECT_EQ(bytes(destination), "header");
    std::filesystem::rename(destination, moved);
    write(destination, "header");
    EXPECT_THROW(
        (io::CheckpointOutputPublication(
            {{stage, destination, "header", io::checkpointFileDescriptor(original.get())}})),
        std::runtime_error);
    EXPECT_EQ(bytes(destination), "header");
    EXPECT_EQ(bytes(stage), "prefix");
    std::filesystem::remove(destination);
    std::filesystem::rename(moved, destination);
    {
        io::CheckpointOutputPublication publication(
            {{stage, destination, "header", io::checkpointFileDescriptor(original.get())}});
        publication.publish();
    }
    EXPECT_EQ(bytes(destination), "prefix");
    ASSERT_EQ(std::fwrite("-continued", 1, 10, prepared.get()), 10u);
    ASSERT_EQ(std::fflush(prepared.get()), 0);
    EXPECT_EQ(bytes(destination), "prefix-continued");
}

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, BinaryOutputContinuesMeshAndPairedSpatialHistories) {
    for (int lid = 0; lid < 3; ++lid)
        for (double cadence : {.7, .31})
            for (bool returns : {false, true}) {
                const auto name = "binary-spatial-" + std::to_string(lid) + "-" +
                                  std::to_string(cadence) + "-" + std::to_string(returns);
                SCOPED_TRACE(name);
                SurfaceSession a, b, control;
                twoD::EngineSourceResultHistory history, resumed;
                ASSERT_NO_THROW(startClosedSpatial(a, name, lid, cadence, returns, true, true));
                ASSERT_NO_THROW(startClosedSpatial(control, name + "-control", lid, cadence,
                                                   returns, true, true));
                ASSERT_EQ(a.engine.setEngineSourceResultHistory(history, true), "");
                while (a.engine.context().current_time < 7.1)
                    ASSERT_NO_THROW(step(a.engine));
                const auto checkpoint = root() / (name + ".chk");
                ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
                resumed = history;
                ASSERT_NO_THROW(
                    startClosedSpatial(b, name + "-resumed", lid, cadence, returns, true, true));
                ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
                ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed), "");
                compare(a.engine, b.engine, name + "-restored");
                sameEngineHistory(history, resumed);
                ASSERT_NO_THROW(finishBinary(a.engine));
                ASSERT_NO_THROW(finishBinary(b.engine));
                ASSERT_NO_THROW(finishBinary(control.engine));
                sameBinary(name, name + "-resumed", 10);
                sameBinary(name, name + "-control", 10);
                sameEngineHistory(history, resumed);
            }
    for (bool spatial : {false, true}) {
        const auto name = std::string("binary-mesh-vector-") + (spatial ? "spatial" : "mesh");
        SurfaceSession a, b;
        ASSERT_NO_THROW(a.start(name, 3, 0, 1, 6, true, spatial, spatial, spatial ? 2 : -1, true));
        while (a.engine.context().current_time < 17.1)
            ASSERT_NO_THROW(step(a.engine));
        const auto checkpoint = root() / (name + ".chk");
        ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
        ASSERT_NO_THROW(
            b.start(name + "-resumed", 3, 0, 1, 6, true, spatial, spatial, spatial ? 2 : -1, true));
        ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
        compare(a.engine, b.engine, name + "-restored");
        for (int k = 0; k < 12; ++k) {
            ASSERT_NO_THROW(step(a.engine));
            ASSERT_NO_THROW(step(b.engine));
        }
        compare(a.engine, b.engine, name + "-continued");
        ASSERT_EQ(a.engine.end(), 0);
        ASSERT_EQ(b.engine.end(), 0);
        EXPECT_EQ(bytes(root() / (name + ".out")), bytes(root() / (name + "-resumed.out")));
    }
}
TEST(EngineCheckpoint, BinaryOutputPublicationFailurePreservesAllSpatialComponents) {
    SurfaceSession a, b;
    ASSERT_NO_THROW(
        startClosedSpatial(a, "binary-spatial-publication-a", 2, .31, true, true, true));
    ASSERT_NO_THROW(
        startClosedSpatial(b, "binary-spatial-publication-b", 2, .31, true, true, true));
    while (a.engine.context().current_time < 9.1)
        ASSERT_NO_THROW(step(a.engine));
    const auto good = root() / "binary-spatial-publication.chk",
               before = root() / "binary-spatial-publication-before.chk",
               output = root() / "binary-spatial-publication-b.out";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    const auto saved = bytes(before), header = bytes(output);
    std::filesystem::permissions(output, std::filesystem::perms::owner_read);
    EXPECT_FALSE(b.engine.loadCheckpoint(good.string()).empty());
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(before), saved);
    EXPECT_EQ(bytes(output), header);
    EXPECT_EQ(b.stage.intervals(), 0);
    std::filesystem::permissions(output, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_write);
    ASSERT_EQ(b.engine.loadCheckpoint(good.string()), "");
    compare(a.engine, b.engine, "binary-spatial-publication-retry");
    ASSERT_EQ(a.engine.end(), 0);
    ASSERT_EQ(b.engine.end(), 0);
}
#endif

#ifndef __EMSCRIPTEN__
TEST(EngineCheckpoint, OutputBarrierWaitsForActiveWriteAndQueuedWritesAndReportsFailure) {
    PluginFactory factory;
    auto* output = new HeldOutput;
    factory.add_output_plugin(output);
    IOThread io(factory);
    io.start();
    io.post(SimulationSnapshot{});
    {
        std::unique_lock lock(output->mutex);
        EXPECT_TRUE(output->changed.wait_for(lock, std::chrono::seconds(5),
                                             [&] { return output->entered; }));
    }
    auto barrier = std::async(std::launch::async, [&] { return io.flush(); });
    EXPECT_EQ(barrier.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    io.post(SimulationSnapshot{});
    {
        std::lock_guard lock(output->mutex);
        output->released = true;
    }
    output->changed.notify_all();
    EXPECT_EQ(barrier.get(), -17);
    EXPECT_EQ(io.tasks_completed(), 2);
    EXPECT_EQ(io.flush(), -17);
    io.stop();
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, BinaryOutputPreservesExistingSourceBatchSchedule) {
    for (double cadence : {.7, .31}) {
        const auto name = "binary-source-schedule-" + std::to_string(cadence);
        SurfaceSession binary, ordinary;
        ASSERT_NO_THROW(startClosedSpatial(binary, name, 2, cadence, true, true, true));
        ASSERT_NO_THROW(
            startClosedSpatial(ordinary, name + "-ordinary", 2, cadence, true, true, false));
        while (binary.engine.context().current_time < 12.1) {
            ASSERT_NO_THROW(step(binary.engine));
            ASSERT_NO_THROW(step(ordinary.engine));
        }
        const auto a = root() / (name + ".chk"), b = root() / (name + "-ordinary.chk");
        ASSERT_EQ(binary.engine.saveCheckpoint(a.string()), "");
        ASSERT_EQ(ordinary.engine.saveCheckpoint(b.string()), "");
        detail::CompletedSourceArchive in(bytes(a));
        std::string magic, model, payload;
        uint32_t version, sum;
        in(magic, version, model, sum, payload);
        in.finish();
        ASSERT_EQ(version, 108u);
        const int marker = 516114522;
        const std::string native(reinterpret_cast<const char*>(&marker), 4);
        const auto body = payload.rfind(native);
        ASSERT_NE(body, std::string::npos);
        ASSERT_GE(body, 24u);
        payload.resize(body - 24);
        version -= 3;
        sum = crc(payload);
        detail::CompletedSourceArchive out;
        out(magic, version, model, sum, payload);
        EXPECT_EQ(out.bytes(), bytes(b));
        EXPECT_EQ(binary.stage.intervals(), ordinary.stage.intervals());
        EXPECT_EQ(binary.stage.completedEnd(), ordinary.stage.completedEnd());
        ASSERT_EQ(binary.engine.end(), 0);
        ASSERT_EQ(ordinary.engine.end(), 0);
    }
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, ValidBinaryPrefixIsNotPublishedWhenSpatialCheckpointValidationFails) {
    SurfaceSession a, b;
    ASSERT_NO_THROW(startClosedSpatial(a, "binary-spatial-invalid-a", 2, .31, true, true, true));
    ASSERT_NO_THROW(startClosedSpatial(b, "binary-spatial-invalid-b", 2, .31, true, true, true));
    while (a.engine.context().current_time < 9.1)
        ASSERT_NO_THROW(step(a.engine));
    const auto good = root() / "binary-spatial-invalid-good.chk",
               bad = root() / "binary-spatial-invalid-bad.chk",
               before = root() / "binary-spatial-invalid-before.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    detail::CompletedSourceArchive in(bytes(good));
    std::string magic, model, payload;
    uint32_t version, sum;
    in(magic, version, model, sum, payload);
    in.finish();
    auto surface = engineSurface(bytes(good));
    auto body = surfacePayload(surface);
    const std::string cpu_magic = "OpenSWMM CPU surface/source boundary";
    auto cpu = nested(body, cpu_magic).second;
    cpu.back() ^= 1;
    surface = withSurfacePayload(surface, replaceNested(body, cpu_magic, cpu));
    payload = replaceNested(payload, "OpenSWMM joint DW surface/source boundary", surface);
    sum = crc(payload);
    detail::CompletedSourceArchive out;
    out(magic, version, model, sum, payload);
    write(bad, out.bytes());
    const auto destination = root() / "binary-spatial-invalid-b.out";
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    const auto saved = bytes(before), header = bytes(destination);
    EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(before), saved);
    EXPECT_EQ(bytes(destination), header);
    for (const auto& file : std::filesystem::directory_iterator(root()))
        EXPECT_FALSE(file.path().filename().string().starts_with(
            "binary-spatial-invalid-b.out.report-part-"));
    ASSERT_EQ(b.engine.loadCheckpoint(good.string()), "");
    compare(a.engine, b.engine, "binary-spatial-invalid-retry");
    ASSERT_EQ(a.engine.end(), 0);
    ASSERT_EQ(b.engine.end(), 0);
}
#endif

#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
namespace {
struct MeshOutputWire {
    uint32_t version = 1;
    double step = 0;
    hsize_t count = 0;
    double due = -1;
    bool groundwater = false;
    std::string image;
    /**
     * @brief Transfer the test mesh-output wire record in its fixed field order.
     *
     * Serializes version, reporting step seconds, count, next due seconds, GW flag and owned image
     * bytes. Reads replace corresponding members; writes append current values. Does not call finish or
     * validate metadata/checksum; archive errors propagate.
     *
     * @param[in,out] a Caller-owned archive selecting read or write direction; advances its
     *     cursor/buffer.
     */
    void io(detail::CompletedSourceArchive& a) { a(version, step, count, due, groundwater, image); }
};
/**
 * @brief Strip the known mesh-output envelope from a physical engine checkpoint copy.
 *
 * Finds the last HDF5 magic and infers fixed native-layout offsets before decoding MeshOutputWire.
 * Throws when magic absent or decoding fails; assumes sufficient prefix bytes/version and does not
 * validate arbitrary archives. Does not inspect HDF5 datasets or alter files.
 *
 * @param[in] checkpoint Owned-by-value engine checkpoint bytes matching the fixture mesh-output
 *     layout.
 * @return Owned physical-only checkpoint with the output tail removed, version decreased by six and
 *     payload CRC recomputed.
 */
std::string meshPhysical(std::string checkpoint) {
    detail::CompletedSourceArchive in(std::move(checkpoint));
    std::string magic, model, payload;
    uint32_t version, sum;
    in(magic, version, model, sum, payload);
    in.finish();
    const auto image = payload.rfind(std::string("\211HDF\r\n\032\n", 8));
    if (image == std::string::npos)
        throw std::runtime_error("Absent mesh checkpoint image");
    const auto begin = image - sizeof(uint64_t) - sizeof(bool) - sizeof(double) - sizeof(hsize_t) -
                       sizeof(double) - sizeof(uint32_t);
    MeshOutputWire wire;
    detail::CompletedSourceArchive tail(payload.substr(begin));
    wire.io(tail);
    tail.finish();
    payload.resize(begin - sizeof(uint64_t));
    version -= 6;
    sum = crc(payload);
    detail::CompletedSourceArchive out;
    out(magic, version, model, sum, payload);
    return out.bytes();
}
/**
 * @brief Save two engine boundaries and compare physical state excluding mesh-output tails.
 *
 * Fatally requires both saves then nonfatally compares meshPhysical(physicalCheckpoint(bytes)).
 * Intentionally excludes output image/session wrapper records, which separate output checks cover.
 * Files may be overwritten; decode/filesystem exceptions propagate.
 *
 * @param[in,out] a Reference engine at a checkpointable completed boundary.
 * @param[in,out] b Candidate engine at the expected same physical boundary.
 * @param[in] name Raw evidence stem below root for -a.chk/-b.chk artifacts.
 */
void compareMesh(SWMMEngine& a, SWMMEngine& b, const std::string& name) {
    const auto x = root() / (name + "-a.chk"), y = root() / (name + "-b.chk");
    ASSERT_EQ(a.saveCheckpoint(x.string()), "");
    ASSERT_EQ(b.saveCheckpoint(y.string()), "");
    EXPECT_EQ(meshPhysical(physicalCheckpoint(bytes(x))),
              meshPhysical(physicalCheckpoint(bytes(y))));
}
/**
 * @brief Find the first active built-in mesh output plugin on the engine.
 *
 * Throws runtime_error when no matching plugin exists. Does not transfer ownership or ensure the
 * writer is idle; callers coordinate lifecycle/output access.
 *
 * @param[in] e Borrowed live engine with a populated output plugin catalogue.
 * @return Borrowed Default 2DOutputPlugin reference valid while the engine retains that plugin.
 */
twoD::Default2DOutputPlugin& meshWriter(SWMMEngine& e) {
    for (auto* p : e.plugin_factory().output_plugins())
        if (auto* w = dynamic_cast<twoD::Default2DOutputPlugin*>(p))
            return *w;
    throw std::runtime_error("Absent mesh writer");
}
/**
 * @brief Compare numeric root datasets in two completed mesh HDF5 output artifacts.
 *
 * Requires closed/quiescent valid fixture files with root dataset names fitting 256 bytes and
 * numeric values convertible to double. Fatally checks opens, enumeration/existence/reads,
 * nonfatally compares root link counts/dataset sizes/time count and exact numeric values with
 * paired NaNs accepted. Groups/attributes/datatype identity and dataset shape beyond element count
 * are not compared. Successful path closes HDF5 IDs; fatal returns can bypass local raw-ID cleanup.
 *
 * @param[in] a Raw first artifact stem below root; .h5 appended.
 * @param[in] b Raw comparison artifact stem below root; .h5 appended.
 * @param[in] periods Expected time-dataset element count.
 */
void sameMesh(const std::string& a, const std::string& b, int periods) {
    hid_t x = H5Fopen((root() / (a + ".h5")).string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    hid_t y = H5Fopen((root() / (b + ".h5")).string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    ASSERT_GE(x, 0);
    ASSERT_GE(y, 0);
    H5G_info_t xi{}, yi{};
    ASSERT_GE(H5Gget_info(x, &xi), 0);
    ASSERT_GE(H5Gget_info(y, &yi), 0);
    EXPECT_EQ(xi.nlinks, yi.nlinks);
    for (hsize_t k = 0; k < xi.nlinks; ++k) {
        char name[256];
        ASSERT_GE(H5Lget_name_by_idx(x, ".", H5_INDEX_NAME, H5_ITER_INC, k, name, sizeof(name),
                                     H5P_DEFAULT),
                  0);
        ASSERT_GT(H5Lexists(y, name, H5P_DEFAULT), 0);
        H5O_info_t info{};
        ASSERT_GE(H5Oget_info_by_name(x, name, &info, H5O_INFO_BASIC, H5P_DEFAULT), 0);
        if (info.type != H5O_TYPE_DATASET)
            continue;
        hid_t xd = H5Dopen2(x, name, H5P_DEFAULT), yd = H5Dopen2(y, name, H5P_DEFAULT);
        hid_t xs = H5Dget_space(xd), ys = H5Dget_space(yd);
        const auto n = H5Sget_simple_extent_npoints(xs);
        EXPECT_EQ(n, H5Sget_simple_extent_npoints(ys));
        if (std::string(name) == "time")
            EXPECT_EQ(n, periods);
        std::vector<double> xv(n), yv(n);
        ASSERT_GE(H5Dread(xd, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, xv.data()), 0);
        ASSERT_GE(H5Dread(yd, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, yv.data()), 0);
        for (std::size_t i = 0; i < xv.size(); ++i)
            if (std::isnan(xv[i]))
                EXPECT_TRUE(std::isnan(yv[i])) << name;
            else
                EXPECT_EQ(xv[i], yv[i]) << name;
        H5Sclose(xs);
        H5Sclose(ys);
        H5Dclose(xd);
        H5Dclose(yd);
    }
    H5Fclose(x);
    H5Fclose(y);
}
}
TEST(EngineCheckpoint, MappedSigmaMeshOutputContinuesIndependentLayerAxesAndReportCalendar) {
    for (const auto counts : {std::pair{5, 3}, std::pair{3, 7}})
        for (double boundary : {0., 31.1}) {
            const auto name = "mapped-output-" + std::to_string(counts.first) + "-" +
                              std::to_string(counts.second) + "-" + std::to_string(boundary);
            SCOPED_TRACE(name);
            ThermalSurfaceSession source, target;
            ASSERT_NO_THROW(startMappedSigmaSession(source, name + "-a", counts.first,
                                                    counts.second, true, "", true, true));
            ASSERT_NO_THROW(startMappedSigmaSession(target, name + "-b", counts.first,
                                                    counts.second, true, "", true, true));
            while (source.engine.context().current_time < boundary)
                ASSERT_NO_THROW(step(source.engine));
            const auto path = root() / (name + ".chk");
            ASSERT_EQ(source.engine.saveCheckpoint(path.string()), "");
            ASSERT_EQ(target.engine.loadCheckpoint(path.string()), "");
            compareMesh(source.engine, target.engine, name + "-restored");
            while (source.engine.context().current_time < 83.1) {
                ASSERT_NO_THROW(step(source.engine));
                ASSERT_NO_THROW(step(target.engine));
            }
            compareMesh(source.engine, target.engine, name + "-continued");
            ASSERT_EQ(source.engine.end(), 0);
            ASSERT_EQ(target.engine.end(), 0);
            sameMesh(name + "-a", name + "-b", 12);
            hid_t file =
                H5Fopen((root() / (name + "-a.h5")).string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
            ASSERT_GE(file, 0);
            for (const auto dataset :
                 {std::pair{"Mesh2_face_gw_layer_mass", counts.first},
                  std::pair{"Mesh2_face_gw_layer_conc", counts.first},
                  std::pair{"Mesh2_face_soil_layer_temperature", counts.second}}) {
                hid_t data = H5Dopen2(file, dataset.first, H5P_DEFAULT);
                ASSERT_GE(data, 0) << dataset.first;
                hid_t space = H5Dget_space(data);
                hsize_t dims[4]{};
                const int rank = H5Sget_simple_extent_dims(space, dims, nullptr);
                ASSERT_GE(rank, 3);
                EXPECT_EQ(dims[rank - 2], dataset.second) << dataset.first;
                H5Sclose(space);
                H5Dclose(data);
            }
            H5Fclose(file);
        }
}

TEST(EngineCheckpoint, MeshFreshOutputRestoresAnInitialCheckpointWithoutChangingItsHeader) {
    SurfaceSession a, b;
    ASSERT_NO_THROW(startClosedSpatial(a, "mesh-fresh-settle-a", 2, .7, true, true, false, 2));
    ASSERT_NO_THROW(startClosedSpatial(b, "mesh-fresh-settle-b", 2, .7, true, true, false, 2));
    const auto header = bytes(root() / "mesh-fresh-settle-b.h5");
    const auto checkpoint = root() / "mesh-fresh-settle.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
    const auto before = root() / "mesh-fresh-settle-before.chk";
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(root() / "mesh-fresh-settle-b.h5"), header);
    ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
    compare(a.engine, b.engine, "mesh-fresh-settle-restored");
}
TEST(EngineCheckpoint, MeshOutputContinuationIsIndependentOfWallClockDelay) {
    SurfaceSession a, b;
    ASSERT_NO_THROW(startClosedSpatial(a, "mesh-wall-clock-a", 2, .7, true, true, false, 2));
    ASSERT_NO_THROW(startClosedSpatial(b, "mesh-wall-clock-b", 2, .7, true, true, false, 2));
    while (a.engine.context().current_time < 7.1)
        ASSERT_NO_THROW(step(a.engine));
    const auto checkpoint = root() / "mesh-wall-clock.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
    ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
    while (a.engine.context().current_time < 16.1)
        ASSERT_NO_THROW(step(a.engine));
    // Continuation must preserve physical state and reported values across
    // wall-clock delays. HDF5 compression/allocation bytes are not a data identity.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    while (b.engine.context().current_time < 16.1)
        ASSERT_NO_THROW(step(b.engine));
    compareMesh(a.engine, b.engine, "mesh-wall-clock-continued");
    while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(a.engine));
        ASSERT_NO_THROW(step(b.engine));
    }
    ASSERT_EQ(a.engine.end(), 0);
    ASSERT_EQ(b.engine.end(), 0);
    sameMesh("mesh-wall-clock-a", "mesh-wall-clock-b", 5);
}

TEST(EngineCheckpoint, MeshOutputContinuesCadenceGroundwaterAndCurrentNodeLIDStores) {
    for (int lid = 0; lid < 3; ++lid)
        for (int format = 1; format < 5; ++format)
            for (double boundary : {0., 2.1, 3., 7.1}) {
                const auto name = "mesh-closed-" + std::to_string(lid) + "-" +
                                  std::to_string(format) + "-" + std::to_string(boundary);
                SCOPED_TRACE(name);
                SurfaceSession a, b;
                ASSERT_NO_THROW(startClosedSpatial(a, name, lid, .7, true, true, false, format));
                ASSERT_NO_THROW(
                    startClosedSpatial(b, name + "-resumed", lid, .7, true, true, false, format));
                while (a.engine.context().current_time < boundary)
                    ASSERT_NO_THROW(step(a.engine));
                const auto checkpoint = root() / (name + ".chk");
                ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
                ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
                compareMesh(a.engine, b.engine, name + "-restored");
                int steps = 0;
                while (a.engine.context().elapsed_ms <
                       a.engine.context().options.totalDurationMs()) {
                    ASSERT_NO_THROW(step(a.engine));
                    ASSERT_NO_THROW(step(b.engine));
                    if (++steps % 5 == 0)
                        compareMesh(a.engine, b.engine, name + "-continued");
                }
                compareMesh(a.engine, b.engine, name + "-final");
                ASSERT_EQ(a.engine.end(), 0);
                ASSERT_EQ(b.engine.end(), 0);
                sameMesh(name, name + "-resumed", format == 4 ? 3 : 5);
            }
}
TEST(EngineCheckpoint, MeshOutputContinuesQueuedSourceHistoryAndPairedAcceptedHistory) {
    for (double cadence : {.7, .31}) {
        const auto name = "mesh-history-" + std::to_string(cadence);
        SurfaceSession a, b;
        twoD::EngineSourceResultHistory history, resumed;
        ASSERT_NO_THROW(startClosedSpatial(a, name, 2, cadence, true, true, false, 2));
        ASSERT_EQ(a.engine.setEngineSourceResultHistory(history, true), "");
        while (a.engine.context().current_time < 7.1) {
            meshWriter(a.engine).enqueueSourceHistory(
                std::make_shared<twoD::SourceResultHistory>(*history.sourceIntervals()));
            ASSERT_NO_THROW(step(a.engine));
        }
        const auto checkpoint = root() / (name + ".chk");
        ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
        resumed = history;
        ASSERT_NO_THROW(startClosedSpatial(b, name + "-resumed", 2, cadence, true, true, false, 2));
        ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
        ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed), "");
        while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
            meshWriter(a.engine).enqueueSourceHistory(
                std::make_shared<twoD::SourceResultHistory>(*history.sourceIntervals()));
            meshWriter(b.engine).enqueueSourceHistory(
                std::make_shared<twoD::SourceResultHistory>(*resumed.sourceIntervals()));
            ASSERT_NO_THROW(step(a.engine));
            ASSERT_NO_THROW(step(b.engine));
            sameEngineHistory(history, resumed);
            compareMesh(a.engine, b.engine, name + "-continued");
        }
        ASSERT_EQ(a.engine.end(), 0);
        ASSERT_EQ(b.engine.end(), 0);
        sameMesh(name, name + "-resumed", 5);
        twoD::SourceResultHistory left, right;
        bool available = false;
        ASSERT_EQ(
            twoD::SourceResultHistory::read((root() / (name + ".h5")).string(), left, available),
            "");
        ASSERT_TRUE(available);
        ASSERT_EQ(twoD::SourceResultHistory::read((root() / (name + "-resumed.h5")).string(), right,
                                                  available),
                  "");
        ASSERT_TRUE(available);
        ASSERT_EQ(left.tables().size(), right.tables().size());
        for (const auto& [n, t] : left.tables())
            EXPECT_EQ(t.columns, right.tables().at(n).columns) << n;
    }
}

namespace {
/**
 * @brief Forge mesh-output envelope or HDF5 schema/value inconsistencies inside a checkpoint.
 *
 * For kinds >=6, writes the embedded image to a retained evidence file, opens it read/write,
 * applies selected HDF5 edits and copies its resulting bytes back. Requires known mesh
 * marker/native layout, populated selected datasets and safe nonempty shapes; most HDF5
 * suboperation statuses are unchecked. Open failure throws, raw ID cleanup is not exception-safe,
 * and partial evidence mutations can remain. Values outside documented selectors still round-trip
 * the image without a targeted edit; not a general arbitrary-archive editing API.
 *
 * @param[in] original Borrowed complete known-layout engine archive with final embedded mesh-output
 *     image.
 * @param[in] name Raw evidence stem below root for name-mutant.h5, overwritten by HDF5 mutation
 *     cases.
 * @param[in] kind 0 count, 1 due time, 2 cadence, 3 GW flag, 4 wire version, 5 image truncation; 6
 *     time shift, 7 depth NaN, 8 node-x shift, 9 missing head, 10 depth extent, 11 missing
 *     Conventions, 12 extra dataset, 13 FLOAT32 depth type, 14 external depth link, 15 empty source
 *     history, 16 external history link, 17 GW bed shift, 18 ledger infinity, 19 layer-mass NaN, 20
 *     layer-concentration infinity, 21 missing units, 22 layer extent, 23 missing species names, 24
 *     ET-stress infinity.
 * @return Owned changed archive with mesh record and enclosing payload CRC re-encoded.
 */
std::string changeMesh(const std::string& original, const std::string& name, int kind) {
    detail::CompletedSourceArchive in(original);
    std::string magic, model, payload;
    uint32_t version, sum;
    in(magic, version, model, sum, payload);
    in.finish();
    const auto at = payload.rfind(std::string("\211HDF\r\n\032\n", 8));
    const auto begin = at - sizeof(uint64_t) - sizeof(bool) - sizeof(double) - sizeof(hsize_t) -
                       sizeof(double) - sizeof(uint32_t);
    MeshOutputWire wire;
    detail::CompletedSourceArchive tail(payload.substr(begin));
    wire.io(tail);
    tail.finish();
    switch (kind) {
    case 0:
        ++wire.count;
        break;
    case 1:
        wire.due += .01;
        break;
    case 2:
        wire.step *= 2;
        break;
    case 3:
        wire.groundwater = !wire.groundwater;
        break;
    case 4:
        ++wire.version;
        break;
    case 5:
        wire.image.resize(9);
        break;
    default: {
        const auto file = root() / (name + "-mutant.h5");
        write(file, wire.image);
        hid_t h5 = H5Fopen(file.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        if (h5 < 0)
            throw std::runtime_error("Cannot edit test HDF5 prefix");
        auto change = [&](const char* path, double value, bool add = false) {
            hid_t d = H5Dopen2(h5, path, H5P_DEFAULT), sp = H5Dget_space(d);
            std::vector<double> v(H5Sget_simple_extent_npoints(sp));
            H5Dread(d, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, v.data());
            v[0] = add ? v[0] + value : value;
            H5Dwrite(d, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, v.data());
            H5Sclose(sp);
            H5Dclose(d);
        };
        switch (kind) {
        case 6:
            change("time", .001, true);
            break;
        case 7:
            change("Mesh2_face_depth", std::numeric_limits<double>::quiet_NaN());
            break;
        case 8:
            change("Mesh2_node_x", 1, true);
            break;
        case 9:
            H5Ldelete(h5, "Mesh2_face_head", H5P_DEFAULT);
            break;
        case 10: {
            hid_t d = H5Dopen2(h5, "Mesh2_face_depth", H5P_DEFAULT);
            hsize_t dims[2] = {wire.count + 1, 2};
            H5Dset_extent(d, dims);
            H5Dclose(d);
            break;
        }
        case 11:
            H5Adelete(h5, "Conventions");
            break;
        case 12: {
            hid_t space = H5Screate(H5S_SCALAR);
            hid_t d = H5Dcreate2(h5, "unexpected", H5T_NATIVE_DOUBLE, space, H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT);
            H5Dclose(d);
            H5Sclose(space);
            break;
        }
        case 13: {
            H5Ldelete(h5, "Mesh2_face_depth", H5P_DEFAULT);
            hsize_t dims[2] = {wire.count, 2};
            hid_t space = H5Screate_simple(2, dims, nullptr);
            hid_t d = H5Dcreate2(h5, "Mesh2_face_depth", H5T_NATIVE_FLOAT, space, H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT);
            H5Dclose(d);
            H5Sclose(space);
            break;
        }
        case 14:
            H5Ldelete(h5, "Mesh2_face_depth", H5P_DEFAULT);
            H5Lcreate_external("absent-external-file.h5", "depth", h5, "Mesh2_face_depth",
                               H5P_DEFAULT, H5P_DEFAULT);
            break;
        case 15: {
            hid_t g = H5Gcreate2(h5, "source_history", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
            H5Gclose(g);
            break;
        }
        case 16: {
            hid_t g = H5Gcreate2(h5, "source_history", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
            H5Lcreate_external("absent-external-file.h5", "history", g, "boundaries", H5P_DEFAULT,
                               H5P_DEFAULT);
            H5Gclose(g);
            break;
        }
        case 17:
            change("Mesh2_face_gw_bed_elev", 1, true);
            break;
        case 18:
            change("groundwater_ledger", std::numeric_limits<double>::infinity());
            break;
        case 19:
            change("Mesh2_face_gw_layer_mass", std::numeric_limits<double>::quiet_NaN());
            break;
        case 20:
            change("Mesh2_face_gw_layer_conc", std::numeric_limits<double>::infinity());
            break;
        case 21: {
            hid_t d = H5Dopen2(h5, "Mesh2_face_gw_layer_mass", H5P_DEFAULT);
            H5Adelete(d, "species_units");
            H5Dclose(d);
            break;
        }
        case 22: {
            hid_t d = H5Dopen2(h5, "Mesh2_face_gw_layer_mass", H5P_DEFAULT);
            hsize_t dims[4] = {wire.count, 3, 4, 2};
            H5Dset_extent(d, dims);
            H5Dclose(d);
            break;
        }
        case 23: {
            hid_t d = H5Dopen2(h5, "Mesh2_face_gw_layer_conc", H5P_DEFAULT);
            H5Adelete(d, "species_names");
            H5Dclose(d);
            break;
        }
        case 24:
            change("Mesh2_face_et_stress", std::numeric_limits<double>::infinity());
            break;
        }
        H5Fclose(h5);
        wire.image = bytes(file);
        break;
    }
    }
    detail::CompletedSourceArchive out_tail;
    wire.io(out_tail);
    std::string output = out_tail.bytes();
    detail::CompletedSourceArchive length;
    length(output);
    payload.resize(begin - sizeof(uint64_t));
    payload += length.bytes();
    sum = crc(payload);
    detail::CompletedSourceArchive out;
    out(magic, version, model, sum, payload);
    return out.bytes();
}
}
TEST(EngineCheckpoint, MappedSigmaOutputRefusesCorruptChemistryBeforePublishingOrInstallingState) {
    ThermalSurfaceSession source;
    ASSERT_NO_THROW(startMappedSigmaSession(source, "mapped-output-refusal-source", 5, 3, true));
    while (source.engine.context().current_time < 31.1)
        ASSERT_NO_THROW(step(source.engine));
    const auto good = root() / "mapped-output-refusal-good.chk";
    ASSERT_EQ(source.engine.saveCheckpoint(good.string()), "");
    for (int kind = 19; kind < 25; ++kind) {
        const auto name = "mapped-output-refusal-" + std::to_string(kind);
        SCOPED_TRACE(name);
        ThermalSurfaceSession target;
        ASSERT_NO_THROW(startMappedSigmaSession(target, name, 5, 3, true));
        const auto before = root() / (name + "-before.chk"), bad = root() / (name + "-bad.chk"),
                   output = root() / (name + ".h5");
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()), "");
        const auto saved = bytes(before), header = bytes(output);
        write(bad, changeMesh(bytes(good), name, kind));
        EXPECT_FALSE(target.engine.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), saved);
        EXPECT_EQ(bytes(output), header);
        ASSERT_EQ(target.engine.loadCheckpoint(good.string()), "");
        compareMesh(source.engine, target.engine, name + "-retry");
        ASSERT_EQ(target.engine.end(), 0);
    }
    ASSERT_EQ(source.engine.end(), 0);
}

TEST(EngineCheckpoint, MeshOutputMalformedPrefixesPreserveFreshEngineAndPermitRetry) {
    SurfaceSession a;
    ASSERT_NO_THROW(startClosedSpatial(a, "mesh-invalid-source", 2, .31, true, true, false, 1));
    while (a.engine.context().current_time < 9.1)
        ASSERT_NO_THROW(step(a.engine));
    const auto good = root() / "mesh-invalid-good.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    for (int kind = 0; kind < 19; ++kind) {
        const auto name = "mesh-invalid-" + std::to_string(kind);
        SCOPED_TRACE(name);
        SurfaceSession b;
        ASSERT_NO_THROW(startClosedSpatial(b, name, 2, .31, true, true, false, 1));
        const auto before = root() / (name + "-before.chk"), bad = root() / (name + "-bad.chk"),
                   output = root() / (name + ".h5");
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        const auto saved = bytes(before), header = bytes(output);
        write(bad, changeMesh(bytes(good), name, kind));
        EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), saved);
        EXPECT_EQ(bytes(output), header);
        for (const auto& file : std::filesystem::directory_iterator(root()))
            EXPECT_FALSE(file.path().filename().string().starts_with(name + ".h5.mesh-part-"));
        ASSERT_EQ(b.engine.loadCheckpoint(good.string()), "");
        compareMesh(a.engine, b.engine, name + "-retry");
        ASSERT_EQ(b.engine.end(), 0);
    }
    ASSERT_EQ(a.engine.end(), 0);
}
TEST(EngineCheckpoint, MeshOutputPublicationAndSpatialFailuresPreserveAllTargetComponents) {
    SurfaceSession a;
    ASSERT_NO_THROW(startClosedSpatial(a, "mesh-atomic-source", 2, .31, true, true, false, 2));
    while (a.engine.context().current_time < 9.1)
        ASSERT_NO_THROW(step(a.engine));
    const auto good = root() / "mesh-atomic-good.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    for (bool spatial : {false, true}) {
        const auto name = std::string("mesh-atomic-") + (spatial ? "spatial" : "publication");
        SurfaceSession b;
        ASSERT_NO_THROW(startClosedSpatial(b, name, 2, .31, true, true, false, 2));
        const auto before = root() / (name + "-before.chk"), output = root() / (name + ".h5");
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        const auto saved = bytes(before), header = bytes(output);
        auto checkpoint = good;
        if (spatial) {
            detail::CompletedSourceArchive in(bytes(good));
            std::string magic, model, payload;
            uint32_t version, sum;
            in(magic, version, model, sum, payload);
            in.finish();
            auto surface = engineSurface(bytes(good)), body = surfacePayload(surface);
            const std::string tag = "OpenSWMM CPU surface/source boundary";
            auto cpu = nested(body, tag).second;
            cpu.back() ^= 1;
            surface = withSurfacePayload(surface, replaceNested(body, tag, cpu));
            payload = replaceNested(payload, "OpenSWMM joint DW surface/source boundary", surface);
            sum = crc(payload);
            detail::CompletedSourceArchive out;
            out(magic, version, model, sum, payload);
            checkpoint = root() / (name + "-bad.chk");
            write(checkpoint, out.bytes());
        } else
            std::filesystem::permissions(output, std::filesystem::perms::owner_read);
        EXPECT_FALSE(b.engine.loadCheckpoint(checkpoint.string()).empty());
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), saved);
        EXPECT_EQ(bytes(output), header);
        std::filesystem::permissions(output, std::filesystem::perms::owner_read |
                                                 std::filesystem::perms::owner_write);
        for (const auto& file : std::filesystem::directory_iterator(root()))
            EXPECT_FALSE(file.path().filename().string().starts_with(name + ".h5.mesh-part-"));
        ASSERT_EQ(b.engine.loadCheckpoint(good.string()), "");
        compareMesh(a.engine, b.engine, name + "-retry");
        ASSERT_EQ(b.engine.end(), 0);
    }
    ASSERT_EQ(a.engine.end(), 0);
}
TEST(EngineCheckpoint, MeshOutputProtectsExistingDestinationsAndExclusiveStart) {
    SurfaceSession authored;
    ASSERT_NO_THROW(startClosedSpatial(authored, "mesh-guard", 0, .7, false, false, false, 1));
    ASSERT_EQ(authored.engine.end(), 0);
    ASSERT_EQ(authored.engine.close(), 0);
    const auto inp = root() / "mesh-guard.inp", destination = root() / "mesh-guard.h5";
    write(destination, "existing results");
    SWMMEngine e;
    ASSERT_EQ(e.open(inp.string().c_str(), nullptr, nullptr), 0);
    EXPECT_FALSE(e.prepareSurfaceOutputCheckpointing().empty());
    ASSERT_EQ(e.initialize(), 0);
    EXPECT_FALSE(e.prepareSurfaceOutputCheckpointing().empty());
    EXPECT_EQ(bytes(destination), "existing results");
    std::filesystem::remove(destination);
    ASSERT_EQ(e.prepareSurfaceOutputCheckpointing(), "");
    write(destination, "intervening creator");
    EXPECT_NE(e.start(0), 0);
    EXPECT_EQ(bytes(destination), "intervening creator");
    ASSERT_EQ(e.close(), 0);
    SWMMEngine binary;
    ASSERT_EQ(
        binary.open(inp.string().c_str(), nullptr, (root() / "mesh-guard.out").string().c_str()),
        0);
    ASSERT_EQ(binary.initialize(), 0);
    EXPECT_FALSE(binary.prepareSurfaceOutputCheckpointing().empty());
    EXPECT_FALSE(binary.prepareOutputCheckpointing().empty());
    ASSERT_EQ(binary.close(), 0);
}

TEST(EngineCheckpoint, MeshOutputMappedVectorRichardsAndBackwaterContinueExactly) {
    for (bool spatial : {false, true}) {
        const auto name = std::string("mesh-vector-") + (spatial ? "spatial" : "mesh-owned");
        SurfaceSession a, b;
        ASSERT_NO_THROW(a.start(name, 3, spatial ? 0 : 1, 1, 6, true, spatial, spatial,
                                spatial ? 2 : -1, false, 1));
        ASSERT_NO_THROW(b.start(name + "-resumed", 3, spatial ? 0 : 1, 1, 6, true, spatial, spatial,
                                spatial ? 2 : -1, false, 1));
        while (a.engine.context().current_time < 43.1)
            ASSERT_NO_THROW(step(a.engine));
        const auto checkpoint = root() / (name + ".chk");
        ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
        ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
        compareMesh(a.engine, b.engine, name + "-restored");
        int steps = 0;
        while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a.engine));
            ASSERT_NO_THROW(step(b.engine));
            if (++steps % 19 == 0)
                compareMesh(a.engine, b.engine, name + "-continued");
        }
        compareMesh(a.engine, b.engine, name + "-final");
        ASSERT_EQ(a.engine.end(), 0);
        ASSERT_EQ(b.engine.end(), 0);
        sameMesh(name, name + "-resumed", 40);
    }
}

TEST(EngineCheckpoint, MeshOutputRejectsChangedSourceHistoryOwnershipBeforePublication) {
    SurfaceSession a;
    twoD::EngineSourceResultHistory history;
    ASSERT_NO_THROW(startClosedSpatial(a, "mesh-source-identity", 2, .31, true, true, false, 1));
    ASSERT_EQ(a.engine.setEngineSourceResultHistory(history, true), "");
    while (a.engine.context().current_time < 7.1) {
        meshWriter(a.engine).enqueueSourceHistory(
            std::make_shared<twoD::SourceResultHistory>(*history.sourceIntervals()));
        ASSERT_NO_THROW(step(a.engine));
    }
    const auto good = root() / "mesh-source-identity.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    detail::CompletedSourceArchive in(bytes(good));
    std::string magic, model, payload;
    uint32_t version, sum;
    in(magic, version, model, sum, payload);
    in.finish();
    const auto at = payload.rfind(std::string("\211HDF\r\n\032\n", 8));
    const auto begin = at - sizeof(uint64_t) - sizeof(bool) - sizeof(double) - sizeof(hsize_t) -
                       sizeof(double) - sizeof(uint32_t);
    MeshOutputWire wire;
    detail::CompletedSourceArchive tail(payload.substr(begin));
    wire.io(tail);
    tail.finish();
    const auto image = root() / "mesh-source-identity-mutant.h5";
    write(image, wire.image);
    hid_t file = H5Fopen(image.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
    ASSERT_GE(file, 0);
    hid_t ds = H5Dopen2(file, "source_history/footprints/area", H5P_DEFAULT);
    ASSERT_GE(ds, 0);
    hid_t space = H5Dget_space(ds);
    std::vector<double> areas(H5Sget_simple_extent_npoints(space));
    ASSERT_GE(H5Dread(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, areas.data()), 0);
    areas[0] += .01;
    ASSERT_GE(H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, areas.data()), 0);
    H5Sclose(space);
    H5Dclose(ds);
    H5Fclose(file);
    wire.image = bytes(image);
    detail::CompletedSourceArchive component;
    wire.io(component);
    std::string output = component.bytes();
    detail::CompletedSourceArchive length;
    length(output);
    payload.resize(begin - sizeof(uint64_t));
    payload += length.bytes();
    sum = crc(payload);
    detail::CompletedSourceArchive out;
    out(magic, version, model, sum, payload);
    const auto bad = root() / "mesh-source-identity-bad.chk";
    write(bad, out.bytes());
    SurfaceSession b;
    ASSERT_NO_THROW(
        startClosedSpatial(b, "mesh-source-identity-target", 2, .31, true, true, false, 1));
    const auto before = root() / "mesh-source-identity-before.chk",
               destination = root() / "mesh-source-identity-target.h5";
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    const auto saved = bytes(before), header = bytes(destination);
    EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(before), saved);
    EXPECT_EQ(bytes(destination), header);
    ASSERT_EQ(b.engine.loadCheckpoint(good.string()), "");
    ASSERT_EQ(a.engine.end(), 0);
    ASSERT_EQ(b.engine.end(), 0);
}

TEST(EngineCheckpoint, CombinedOutputContinuesBothReportClocksAndAveragingWindows) {
    for (int lid = 0; lid < 3; ++lid)
        for (int format : {1, 2, 3, 4})
            for (double boundary : {0., 2.1, 3., 7.1}) {
                const auto name = "combined-closed-" + std::to_string(lid) + "-" +
                                  std::to_string(format) + "-" + std::to_string(boundary);
                SCOPED_TRACE(name);
                SurfaceSession a, b;
                const bool average = format % 2 == 0;
                ASSERT_NO_THROW(
                    startClosedSpatial(a, name, lid, .7, true, true, true, format, average));
                ASSERT_NO_THROW(startClosedSpatial(b, name + "-resumed", lid, .7, true, true, true,
                                                   format, average));
                while (a.engine.context().current_time < boundary)
                    ASSERT_NO_THROW(step(a.engine));
                const auto checkpoint = root() / (name + ".chk");
                ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
                ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
                compareMesh(a.engine, b.engine, name + "-restored");
                int steps = 0;
                while (a.engine.context().elapsed_ms <
                       a.engine.context().options.totalDurationMs()) {
                    ASSERT_NO_THROW(step(a.engine));
                    ASSERT_NO_THROW(step(b.engine));
                    if (++steps % 7 == 0)
                        compareMesh(a.engine, b.engine, name + "-continued");
                }
                compareMesh(a.engine, b.engine, name + "-final");
                ASSERT_EQ(a.engine.end(), 0);
                ASSERT_EQ(b.engine.end(), 0);
                sameBinary(name, name + "-resumed", format == 4 ? 6 : 10);
                sameMesh(name, name + "-resumed", format == 4 ? 3 : 5);
            }
}
TEST(EngineCheckpoint, CombinedOutputContinuesPairedHistoriesAndVectorRichardsBackwater) {
    for (int scenario = 0; scenario < 4; ++scenario) {
        const auto name = "combined-paired-" + std::to_string(scenario);
        SurfaceSession a, b;
        const bool spatial = scenario != 2;
        const double cadence = scenario == 0 ? .7 : .31;
        if (scenario < 2) {
            ASSERT_NO_THROW(startClosedSpatial(a, name, 2, cadence, true, true, true, 2, true));
            ASSERT_NO_THROW(
                startClosedSpatial(b, name + "-resumed", 2, cadence, true, true, true, 2, true));
        } else {
            ASSERT_NO_THROW(a.start(name, 3, spatial ? 0 : 1, 1, 6, true, spatial, spatial,
                                    spatial ? 2 : -1, true, 2, true));
            ASSERT_NO_THROW(b.start(name + "-resumed", 3, spatial ? 0 : 1, 1, 6, true, spatial,
                                    spatial, spatial ? 2 : -1, true, 2, true));
        }
        twoD::EngineSourceResultHistory history, resumed;
        if (spatial)
            ASSERT_EQ(a.engine.setEngineSourceResultHistory(history, true), "");
        while (a.engine.context().current_time < (scenario < 2 ? 7.1 : 43.1)) {
            if (spatial)
                meshWriter(a.engine).enqueueSourceHistory(
                    std::make_shared<twoD::SourceResultHistory>(*history.sourceIntervals()));
            ASSERT_NO_THROW(step(a.engine));
        }
        const auto checkpoint = root() / (name + ".chk");
        ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
        ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
        if (spatial) {
            resumed = history;
            ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed), "");
        }
        int steps = 0;
        while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
            if (spatial) {
                meshWriter(a.engine).enqueueSourceHistory(
                    std::make_shared<twoD::SourceResultHistory>(*history.sourceIntervals()));
                meshWriter(b.engine).enqueueSourceHistory(
                    std::make_shared<twoD::SourceResultHistory>(*resumed.sourceIntervals()));
            }
            ASSERT_NO_THROW(step(a.engine));
            ASSERT_NO_THROW(step(b.engine));
            if (++steps % 19 == 0) {
                compareMesh(a.engine, b.engine, name + "-continued");
                if (spatial)
                    sameEngineHistory(history, resumed);
            }
        }
        compareMesh(a.engine, b.engine, name + "-final");
        if (spatial)
            sameEngineHistory(history, resumed);
        ASSERT_EQ(a.engine.end(), 0);
        ASSERT_EQ(b.engine.end(), 0);
        sameBinary(name, name + "-resumed", scenario < 2 ? 10 : 80);
        sameMesh(name, name + "-resumed", scenario < 2 ? 5 : 40);
        if (spatial) {
            twoD::SourceResultHistory x, y;
            bool available = false;
            ASSERT_EQ(
                twoD::SourceResultHistory::read((root() / (name + ".h5")).string(), x, available),
                "");
            ASSERT_TRUE(available);
            ASSERT_EQ(twoD::SourceResultHistory::read((root() / (name + "-resumed.h5")).string(), y,
                                                      available),
                      "");
            ASSERT_TRUE(available);
            for (const auto& [n, t] : x.tables())
                EXPECT_EQ(t.columns, y.tables().at(n).columns) << n;
        }
    }
}
namespace {
/**
 * @brief Apply a binary-record mutant while preserving the combined mesh-output suffix.
 *
 * Temporarily strips mesh via meshPhysical, applies native binary mutation, restores the saved
 * suffix and adds six back to the physical version. Requires exact known combined layout and valid
 * version arithmetic; no file IO beyond delegated helpers and decode/allocation errors propagate.
 *
 * @param[in] original Borrowed full known-layout combined binary/mesh checkpoint.
 * @param[in] mutation Binary mutation selector forwarded to changeBinary, normally 0..8.
 * @return Owned combined archive with changed binary record, restored mesh suffix and recomputed
 *     outer CRC.
 */
std::string changeCombinedBinary(const std::string& original, int mutation) {
    // Binary mutation helper expects the binary suffix; temporarily remove and
    // then restore the separately length-prefixed mesh record.
    const auto physical = meshPhysical(original);
    detail::CompletedSourceArchive x(original);
    std::string magic, model, payload;
    uint32_t version, sum;
    x(magic, version, model, sum, payload);
    x.finish();
    detail::CompletedSourceArchive p(physical);
    std::string m, md, body;
    uint32_t v, c;
    p(m, v, md, c, body);
    p.finish();
    const auto mesh_suffix = payload.substr(body.size());
    detail::CompletedSourceArchive changed(changeBinary(physical, mutation));
    changed(m, v, md, c, body);
    changed.finish();
    body += mesh_suffix;
    v += 6;
    c = crc(body);
    detail::CompletedSourceArchive out;
    out(m, v, md, c, body);
    return out.bytes();
}
/**
 * @brief Assert named binary/mesh artifacts left no transaction staging or backup files.
 *
 * Iterates root evidence directory and nonfatally rejects report-part-, mesh-part- or
 * checkpoint-backup- matches in selected names. Does not delete artifacts or inspect unmatched
 * prefixes; directory errors can propagate.
 *
 * @param[in] name Raw artifact prefix; only filenames beginning name.out. or name.h5. are scanned
 *     for reserved substrings.
 */
void noOutputStaging(const std::string& name) {
    for (const auto& f : std::filesystem::directory_iterator(root())) {
        const auto path = f.path().filename().string();
        if (path.starts_with(name + ".out.") || path.starts_with(name + ".h5."))
            EXPECT_FALSE(path.find("report-part-") != std::string::npos ||
                         path.find("mesh-part-") != std::string::npos ||
                         path.find("checkpoint-backup-") != std::string::npos)
                << path;
    }
}
}
TEST(EngineCheckpoint, CombinedOutputRejectsEitherMalformedPrefixWithoutPublishingTheOther) {
    SurfaceSession a;
    ASSERT_NO_THROW(
        startClosedSpatial(a, "combined-invalid-source", 2, .31, true, true, true, 2, true));
    while (a.engine.context().current_time < 9.1)
        ASSERT_NO_THROW(step(a.engine));
    const auto good = root() / "combined-invalid-good.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    for (int mutation = 0; mutation < 28; ++mutation) {
        const auto name = "combined-invalid-" + std::to_string(mutation);
        SCOPED_TRACE(name);
        SurfaceSession b;
        ASSERT_NO_THROW(startClosedSpatial(b, name, 2, .31, true, true, true, 2, true));
        const auto before = root() / (name + "-before.chk"), bad = root() / (name + "-bad.chk");
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        const auto saved = bytes(before), binary = bytes(root() / (name + ".out")),
                   mesh = bytes(root() / (name + ".h5"));
        write(bad, mutation < 9 ? changeCombinedBinary(bytes(good), mutation)
                                : changeMesh(bytes(good), name, mutation - 9));
        EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), saved);
        EXPECT_EQ(bytes(root() / (name + ".out")), binary);
        EXPECT_EQ(bytes(root() / (name + ".h5")), mesh);
        noOutputStaging(name);
        ASSERT_EQ(b.engine.loadCheckpoint(good.string()), "");
        compareMesh(a.engine, b.engine, name + "-retry");
        ASSERT_EQ(b.engine.end(), 0);
    }
    ASSERT_EQ(a.engine.end(), 0);
}
TEST(EngineCheckpoint, CombinedOutputPublicationAndSpatialFailuresPreserveBothWriters) {
    SurfaceSession a;
    ASSERT_NO_THROW(startClosedSpatial(a, "combined-atomic-source", 2, .31, true, true, true, 2));
    while (a.engine.context().current_time < 9.1)
        ASSERT_NO_THROW(step(a.engine));
    const auto good = root() / "combined-atomic-good.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    for (int failure = 0; failure < 5; ++failure) {
        const auto name = "combined-atomic-" + std::to_string(failure);
        SurfaceSession b;
        ASSERT_NO_THROW(startClosedSpatial(b, name, 2, .31, true, true, true, 2));
        const auto before = root() / (name + "-before.chk"), binary = root() / (name + ".out"),
                   mesh = root() / (name + ".h5");
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        const auto saved = bytes(before), bh = bytes(binary), mh = bytes(mesh);
        auto checkpoint = good;
        if (failure < 2)
            std::filesystem::permissions(failure ? mesh : binary,
                                         std::filesystem::perms::owner_read);
        else if (failure == 2) {
            detail::CompletedSourceArchive in(bytes(good));
            std::string magic, model, payload;
            uint32_t version, sum;
            in(magic, version, model, sum, payload);
            in.finish();
            auto surface = engineSurface(bytes(good)), body = surfacePayload(surface);
            const std::string tag = "OpenSWMM CPU surface/source boundary";
            auto cpu = nested(body, tag).second;
            cpu.back() ^= 1;
            surface = withSurfacePayload(surface, replaceNested(body, tag, cpu));
            payload = replaceNested(payload, "OpenSWMM joint DW surface/source boundary", surface);
            sum = crc(payload);
            detail::CompletedSourceArchive out;
            out(magic, version, model, sum, payload);
            checkpoint = root() / (name + "-bad.chk");
            write(checkpoint, out.bytes());
        }
        const auto changed = failure == 3 ? binary : mesh,
                   original = root() / (name + "-original-header");
        if (failure >= 3) {
            std::filesystem::rename(changed, original);
            write(changed, failure == 3 ? bh : mh);
        }
        EXPECT_FALSE(b.engine.loadCheckpoint(checkpoint.string()).empty());
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), saved);
        EXPECT_EQ(bytes(binary), bh);
        EXPECT_EQ(bytes(mesh), mh);
        noOutputStaging(name);
        if (failure >= 3) {
            std::filesystem::remove(changed);
            std::filesystem::rename(original, changed);
        }
        for (const auto& path : {binary, mesh})
            std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                                   std::filesystem::perms::owner_write);
        ASSERT_EQ(b.engine.loadCheckpoint(good.string()), "");
        compareMesh(a.engine, b.engine, name + "-retry");
        ASSERT_EQ(b.engine.end(), 0);
    }
    ASSERT_EQ(a.engine.end(), 0);
}
TEST(EngineCheckpoint, OutputPublicationRollsBackAnEarlierRenameAndProtectsAliasedPaths) {
    const auto a = root() / "publication-a", b = root() / "publication-b",
               sa = root() / "publication-a-stage", sb = root() / "publication-b-stage";
    write(a, "header-a");
    write(b, "header-b");
    write(sa, "prefix-a");
    write(sb, "prefix-b");
    {
        io::CheckpointOutputPublication publication({{sa, a, "header-a"}, {sb, b, "header-b"}});
        // Simulate loss of the second prepared staging file after preparation.
        std::filesystem::remove(sb);
        EXPECT_THROW(publication.publish(), std::runtime_error);
        EXPECT_EQ(bytes(a), "header-a");
        EXPECT_EQ(bytes(b), "header-b");
    }
    write(sa, "prefix-a");
    write(sb, "prefix-b");
    EXPECT_THROW((io::CheckpointOutputPublication({{sa, a, "header-a"}, {sb, a, "header-a"}})),
                 std::runtime_error);
    const auto alias = root() / "publication-hardlink";
    std::filesystem::remove(alias);
    std::filesystem::create_hard_link(a, alias);
    EXPECT_THROW((io::CheckpointOutputPublication({{sa, a, "header-a"}, {sb, alias, "header-a"}})),
                 std::runtime_error);
    std::filesystem::remove(alias);
    {
        io::CheckpointOutputPublication publication({{sa, a, "header-a"}, {sb, b, "header-b"}});
        publication.publish();
    }
    EXPECT_EQ(bytes(a), "prefix-a");
    EXPECT_EQ(bytes(b), "prefix-b");
    for (const auto& f : std::filesystem::directory_iterator(root()))
        EXPECT_EQ(f.path().filename().string().find("publication-a.checkpoint-backup-"),
                  std::string::npos);
}
TEST(EngineCheckpoint, CombinedOutputPreparationProtectsLifecycleAndBothFreshDestinations) {
    SurfaceSession valid;
    ASSERT_NO_THROW(startClosedSpatial(valid, "combined-guard", 2, .7, true, true, true, 2));
    EXPECT_FALSE(valid.engine.prepareCombinedOutputCheckpointing().empty());
    SurfaceSession mesh;
    ASSERT_NO_THROW(startClosedSpatial(mesh, "combined-mesh-only", 2, .7, true, true, false, 2));
    EXPECT_FALSE(mesh.engine.prepareCombinedOutputCheckpointing().empty());
    SurfaceSession binary;
    ASSERT_NO_THROW(startClosedSpatial(binary, "combined-binary-only", 2, .7, true, true, true));
    EXPECT_FALSE(binary.engine.prepareCombinedOutputCheckpointing().empty());
    const auto geometry = valid.engine.surfaceRouter2D().mesh();
    ASSERT_EQ(valid.engine.end(), 0);
    ASSERT_EQ(valid.engine.close(), 0);
    const auto inp = root() / "combined-guard.inp", destination = root() / "combined-guard.out",
               h5 = root() / "combined-guard.h5";
    SWMMEngine e;
    ASSERT_EQ(e.open(inp.string().c_str(), nullptr, destination.string().c_str()), 0);
    e.surfaceRouter2D().mesh() = geometry;
    const auto binary_header = bytes(destination), mesh_header = bytes(h5);
    EXPECT_FALSE(e.prepareCombinedOutputCheckpointing().empty());
    ASSERT_EQ(e.initialize(), 0);
    EXPECT_FALSE(e.prepareCombinedOutputCheckpointing().empty());
    EXPECT_EQ(bytes(destination), binary_header);
    EXPECT_EQ(bytes(h5), mesh_header);
    std::filesystem::remove(destination);
    EXPECT_FALSE(e.prepareCombinedOutputCheckpointing().empty());
    EXPECT_EQ(bytes(h5), mesh_header);
    std::filesystem::remove(h5);
    ASSERT_EQ(e.prepareCombinedOutputCheckpointing(), "");
    write(h5, "intervening creator");
    EXPECT_NE(e.start(1), 0);
    EXPECT_EQ(bytes(h5), "intervening creator");
    ASSERT_EQ(e.close(), 0);
}

#endif

#ifdef OPENSWMM_HAS_2D
namespace {
/**
 * @brief Start the baseline two-cell runtime-provider continuation profile.
 *
 * Uses reversed triangle/node mapping, tracked nonspatial common-source checkpointing and no
 * outfall-return override. Does not apply a runtime frame itself; SurfaceSession setup exceptions
 * propagate with partial member state retained.
 *
 * @param[in,out] s Fresh owned SurfaceSession whose stage/engine/source references remain alive.
 * @param[in] name Raw evidence stem below root.
 * @param[in] lid NODE LID mode forwarded to SurfaceSession.
 * @param[in] closure Groundwater closure selector 0..2.
 * @param[in] output Enable saved binary output and averaged reporting together.
 * @param[in] mesh_output Mesh-output format selector forwarded unchanged.
 * @param[in] identity Borrowed runtime driver identity copied during stage initialization.
 */
void startProvider(SurfaceSession& s, const std::string& name, int lid = 0, int closure = 0,
                   bool output = false, int mesh_output = 0,
                   const std::string& identity = "mesh water driver revision 1") {
    s.start(name, lid, 1, closure, 0, true, false, false, -1, output, mesh_output, output,
            identity);
}
/**
 * @brief Apply the fixed water-provider mixed source and weather transaction.
 *
 * Publishes provider water with surface/GW cell-zero inflows, cell-one -2 m3/s sinks and four
 * rainfall/PET/infiltration forcing rows in m/s; one cell rain override expires at 7.75 s
 * independently. Stack frame arrays/strings are copied by the public transaction before return.
 * Rejection semantics and any pending-physics flush follow the coupling API; this helper does not
 * assert acceptance.
 *
 * @param[in,out] e Started supported CPU-mesh engine at a permitted between-step boundary.
 * @param[in] until Expiry in elapsed seconds for most source/weather rows.
 * @param[in] incoming Signed surface cell-zero source flow in m3/s.
 * @return swmm_coupling_apply_frame status unchanged.
 */
int providerFrame(SWMMEngine& e, double until = 9.25, double incoming = .01) {
    const SWMM_CouplingSource sources[] = {
        {sizeof(SWMM_CouplingSource), "in", 0, 0, incoming, 0, until, 0, nullptr, nullptr},
        {sizeof(SWMM_CouplingSource), "out", 0, 1, -2., 0, until, 0, nullptr, nullptr},
        {sizeof(SWMM_CouplingSource), "in", 1, 0, .02, 0, until, 0, nullptr, nullptr},
        {sizeof(SWMM_CouplingSource), "out", 1, 1, -2., 0, until, 0, nullptr, nullptr}};
    const SWMM_CouplingForcing weather[] = {
        {sizeof(SWMM_CouplingForcing), 1, -1, 1, .001, until, 0, nullptr},
        {sizeof(SWMM_CouplingForcing), 1, 0, 1, .002, 7.75, 0, nullptr},
        {sizeof(SWMM_CouplingForcing), 2, -1, 2, .0001, until, 0, nullptr},
        {sizeof(SWMM_CouplingForcing), 3, 0, 1, .02, until, 0, nullptr}};
    const SWMM_CouplingFrame frame{sizeof(frame), 1, "water", 4, sources, 0,
                                   nullptr,       4, weather, 0, nullptr};
    return swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&e), &frame);
}
/**
 * @brief Step until the fixture reaches a requested elapsed-time boundary.
 *
 * Uses throwing step helper without an iteration guard or completion check. Caller must choose a
 * target within the run and own later lifecycle cleanup.
 *
 * @param[in,out] e Started engine advanced in place; successful steps must make progress.
 * @param[in] target Reachable elapsed seconds; stop comparison allows 1e-12 seconds and the last
 *     step can overshoot.
 */
void until(SWMMEngine& e, double target) {
    while (e.context().current_time < target - 1e-12)
        step(e);
}
/**
 * @brief Forge runtime-provider identity, state or receipt inconsistency through nested
 * checkpoints.
 *
 * Decodes known provider/source/CPU/surface field layouts and rewraps them; targeted nested
 * corruption remains intentional. Requires nonempty cells/payload/receipt rows and valid
 * marker-derived offsets, beyond explicit archive decoder checks. Not safe for arbitrary inputs;
 * decode/bounds/allocation exceptions can propagate and no model/file is mutated.
 *
 * @param[in] original Borrowed known-layout engine/surface/CPU/source/provider archive bytes.
 * @param[in] mutation 0 changed identity, 1 time +0.125 s, 2 first qualified cell disabled, 3
 *     opaque payload truncation, 4 provider version increment, 5 provider-record truncation, 6
 *     clear pre-CPU profile byte, >=7 increase first surface receipt by 0.001 m3, with exact 8
 *     replacing all surface water-in receipts by 1e308.
 * @return Owned changed engine archive with enclosing payload checksums recomputed.
 */
std::string changeProviderRecord(const std::string& original, int mutation) {
    using A = detail::CompletedSourceArchive;
    A engine(original);
    std::string em, model, payload;
    uint32_t ev, sum;
    engine(em, ev, model, sum, payload);
    engine.finish();
    auto surface = engineSurface(original), surface_data = surfacePayload(surface);
    if (mutation == 6) {
        surface_data[nested(surface_data, "OpenSWMM CPU surface/source boundary").first - 1] = 0;
        payload = replaceNested(payload, "OpenSWMM joint DW surface/source boundary",
                                withSurfacePayload(surface, surface_data));
        sum = crc(payload);
        A result;
        result(em, ev, model, sum, payload);
        return result.bytes();
    }
    auto cpu = nested(surface_data, "OpenSWMM CPU surface/source boundary").second;
    A ca(cpu);
    std::string cm, cp;
    uint32_t cv, endian, long_bytes;
    ca(cm, cv, endian, long_bytes, cp, sum);
    ca.finish();
    auto source = nested(cp, "OpenSWMM source boundary").second;
    A sa(source);
    std::string sm, source_model, sp;
    uint32_t sv, se, sl, sd;
    sa(sm, sv, se, sl, sd, source_model, sp, sum);
    sa.finish();
    auto provider = nested(sp, "OpenSWMM Runtime Water").second;
    A pa(provider);
    std::string pm, identity, pp;
    uint32_t pv, pe, pi, pr;
    std::vector<bool> cells;
    double time;
    pa(pm, pv, pe, pi, pr, identity, cells, time, pp, sum);
    pa.finish();
    if (mutation == 0)
        identity += "changed";
    if (mutation == 1)
        time += .125;
    if (mutation == 2)
        cells[0] = false;
    if (mutation == 3)
        pp.pop_back();
    if (mutation == 4)
        pv++;
    if (mutation >= 7) {
        twoD::RuntimeSources s, g;
        twoD::RuntimeForcings f;
        const auto receipt = [](A& a, twoD::RuntimeReceipt& r) {
            a(r.requested, r.applied, r.water_in, r.water_out, r.last_flow, r.last_dt);
        };
        const auto fields = [&](A& a) {
            const auto row = [&](auto& r) {
                a(r.id, r.owner, r.cell, r.flow, r.until, r.enabled);
                receipt(a, r.receipt);
            };
            a.sequence(s.rows, row);
            a.sequence(g.rows, row);
            a.sequence(f.rows, [&](auto& r) {
                a(r.channel, r.cell, r.mode, r.rate, r.until, r.enabled, r.owner);
                receipt(a, r.receipt);
            });
        };
        A read(pp);
        fields(read);
        read.finish();
        s.rows[0].receipt.water_in += .001;
        s.rows[0].receipt.applied += .001;
        if (mutation == 8)
            for (auto& r : s.rows) {
                r.receipt.water_in = 1e308;
                r.receipt.applied = r.receipt.water_in - r.receipt.water_out;
            }
        A updated;
        fields(updated);
        pp = updated.bytes();
    }
    if (mutation == 5)
        provider.pop_back();
    else {
        sum = crc(pp);
        A po;
        po(pm, pv, pe, pi, pr, identity, cells, time, pp, sum);
        provider = po.bytes();
    }
    sp = replaceNested(sp, "OpenSWMM Runtime Water", provider);
    sum = crc(sp);
    A so;
    so(sm, sv, se, sl, sd, source_model, sp, sum);
    cp = replaceNested(cp, "OpenSWMM source boundary", so.bytes());
    sum = crc(cp);
    A co;
    co(cm, cv, endian, long_bytes, cp, sum);
    surface_data = replaceNested(surface_data, "OpenSWMM CPU surface/source boundary", co.bytes());
    surface = withSurfacePayload(surface, surface_data);
    payload = replaceNested(payload, "OpenSWMM joint DW surface/source boundary", surface);
    sum = crc(payload);
    A result;
    result(em, ev, model, sum, payload);
    return result.bytes();
}
}

TEST(EngineCheckpoint, RuntimeWaterWholeSessionContinuesStoresAndNodeLidProfiles) {
    for (int lid : {0, 1, 2})
        for (int closure : {0, 1, 2}) {
            const auto name =
                "provider-session-" + std::to_string(lid) + "-" + std::to_string(closure);
            SCOPED_TRACE(name);
            SurfaceSession a, b;
            ASSERT_NO_THROW(startProvider(a, name + "-a", lid, closure));
            ASSERT_NO_THROW(startProvider(b, name + "-b", lid, closure));
            ASSERT_EQ(providerFrame(a.engine), SWMM_OK);
            ASSERT_NO_THROW(until(a.engine, 3.1));
            ASSERT_LE(a.stage.completedEnd(), a.engine.context().current_time);
            auto checkpoint = root() / (name + ".chk");
            ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
            ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
            EXPECT_TRUE(b.engine.context().runtime_coupling_used);
            compare(a.engine, b.engine, name + "-restored");
            while (a.engine.context().current_time < 20.125) {
                ASSERT_NO_THROW(step(a.engine));
                ASSERT_NO_THROW(step(b.engine));
                compare(a.engine, b.engine, name + "-step");
            }
            auto& router = a.engine.surfaceRouter2D();
            EXPECT_FALSE(router.runtimeActive());
            const auto* in = router.runtimeSources().find("provider/water/in", 0);
            ASSERT_NE(in, nullptr);
            EXPECT_NEAR(in->receipt.requested, .01 * 9.25, 1e-12);
            EXPECT_GT(in->receipt.water_in, 0);
            const auto* gw = router.subsurface().runtimeSources().find("provider/water/in", 0);
            ASSERT_NE(gw, nullptr);
            EXPECT_NEAR(gw->receipt.requested, .02 * 9.25, 1e-12);
            EXPECT_GT(router.state().runtime_forcings.find(3, 0)->receipt.requested, 0);
            EXPECT_GT(router.runtimeSources().find("provider/water/out", 1)->receipt.water_out, 0);
            EXPECT_LE(router.runtimeSources().find("provider/water/out", 1)->receipt.water_out,
                      2 * 9.25);
        }
}
TEST(EngineCheckpoint, RuntimeWaterExpiryIsAnExactPhysicalBarrierAndInactiveHistoryRestores) {
    SurfaceSession a, b;
    ASSERT_NO_THROW(startProvider(a, "provider-expiry-a"));
    ASSERT_NO_THROW(startProvider(b, "provider-expiry-b"));
    ASSERT_EQ(providerFrame(a.engine), SWMM_OK);
    ASSERT_NO_THROW(until(a.engine, 7.75));
    EXPECT_NEAR(a.engine.context().current_time, 7.75, 1e-12);
    EXPECT_NEAR(a.stage.completedEnd(), 7.75, 1e-12);
    auto& forcing = a.engine.surfaceRouter2D().state().runtime_forcings;
    EXPECT_FALSE(forcing.find(1, 0)->enabled);
    ASSERT_NE(forcing.get(1, 0), nullptr);
    EXPECT_EQ(forcing.get(1, 0)->cell, -1);
    ASSERT_NO_THROW(until(a.engine, 9.25));
    EXPECT_NEAR(a.engine.context().current_time, 9.25, 1e-12);
    EXPECT_NEAR(a.stage.completedEnd(), 9.25, 1e-12);
    EXPECT_FALSE(a.engine.surfaceRouter2D().runtimeActive());
    EXPECT_TRUE(a.engine.context().runtime_coupling_used);
    ASSERT_NO_THROW(step(a.engine));
    ASSERT_LT(a.stage.completedEnd(), a.engine.context().current_time);
    auto checkpoint = root() / "provider-expired.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
    ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
    EXPECT_TRUE(b.engine.context().runtime_coupling_used);
    EXPECT_FALSE(b.engine.surfaceRouter2D().runtimeActive());
    compare(a.engine, b.engine, "provider-expired");
    const auto before = root() / "provider-inactive-before.chk";
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    const auto saved = bytes(before);
    const double prior = b.stage.completedEnd();
    const SWMM_CouplingBoundary boundary{sizeof(boundary), 0, 0, 0, 1, 1., 0, nullptr};
    const SWMM_CouplingFrame unsupported{sizeof(unsupported), 1, "water", 0, nullptr, 1,
                                         &boundary,           0, nullptr, 0, nullptr};
    EXPECT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&b.engine), &unsupported),
              SWMM_ERR_PLUGIN);
    EXPECT_EQ(b.stage.completedEnd(), prior);
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(before), saved);
    for (auto* engine : {&a.engine, &b.engine})
        ASSERT_EQ(swmm_coupling_set_source(reinterpret_cast<SWMM_Engine>(engine), "future", 0, 0,
                                           .01, 0, nullptr, nullptr, 0, 20.125),
                  SWMM_OK);
    EXPECT_EQ(a.stage.completedEnd(), a.engine.context().current_time);
    compare(a.engine, b.engine, "provider-future-barrier");
    ASSERT_NO_THROW(step(a.engine));
    ASSERT_NO_THROW(step(b.engine));
    compare(a.engine, b.engine, "provider-native-fallback");
    const auto& router = b.engine.surfaceRouter2D();
    EXPECT_EQ(router.state().runtime_forcings.rate(1, 0, router.state().rainfall[0]),
              router.state().rainfall[0]);
}
TEST(EngineCheckpoint, RuntimeWaterFutureFramesPreservePastReceiptsAndRejectBeforeFlushing) {
    SurfaceSession a, b;
    ASSERT_NO_THROW(startProvider(a, "provider-update-a"));
    ASSERT_NO_THROW(startProvider(b, "provider-update-b"));
    ASSERT_EQ(providerFrame(a.engine), SWMM_OK);
    ASSERT_NO_THROW(until(a.engine, 4.2));
    auto checkpoint = root() / "provider-before-update.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
    ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
    const auto before = bytes(checkpoint);
    const double source_end = b.stage.completedEnd();
    const SWMM_CouplingBoundary boundary{sizeof(boundary), 0, 0, 0, 1, 1., 0, nullptr};
    const SWMM_CouplingFrame bad{sizeof(bad), 1, "water", 0, nullptr, 1,
                                 &boundary,   0, nullptr, 0, nullptr};
    EXPECT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&b.engine), &bad),
              SWMM_ERR_PLUGIN);
    EXPECT_EQ(b.stage.completedEnd(), source_end);
    ASSERT_EQ(b.engine.saveCheckpoint(checkpoint.string()), "");
    EXPECT_EQ(bytes(checkpoint), before);
    const SWMM_CouplingForcing unsupported{sizeof(unsupported), 4, 0, 1, 0, 0, 0, nullptr};
    const SWMM_CouplingFrame quality{sizeof(quality), 1, "water",      0, nullptr, 0,
                                     nullptr,         1, &unsupported, 0, nullptr};
    EXPECT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&b.engine), &quality),
              SWMM_ERR_PLUGIN);
    EXPECT_EQ(b.stage.completedEnd(), source_end);
    ASSERT_EQ(b.engine.saveCheckpoint(checkpoint.string()), "");
    EXPECT_EQ(bytes(checkpoint), before);
    const double changed_at = a.engine.context().current_time;
    ASSERT_EQ(providerFrame(a.engine, 12.5, .03), SWMM_OK);
    ASSERT_EQ(providerFrame(b.engine, 12.5, .03), SWMM_OK);
    EXPECT_EQ(a.stage.completedEnd(), changed_at);
    compare(a.engine, b.engine, "provider-changed");
    ASSERT_NO_THROW(until(a.engine, 12.5));
    ASSERT_NO_THROW(until(b.engine, 12.5));
    compare(a.engine, b.engine, "provider-changed-expired");
    const auto* row = b.engine.surfaceRouter2D().runtimeSources().find("provider/water/in", 0);
    EXPECT_NEAR(row->receipt.requested, .01 * changed_at + .03 * (12.5 - changed_at), 1e-12);
    EXPECT_NEAR(a.stage.completedEnd(), 12.5, 1e-12);
}
TEST(EngineCheckpoint, RuntimeWaterPairedInfiltrationReplacesRateAndSharesFiniteWater) {
    SurfaceSession a;
    ASSERT_NO_THROW(startProvider(a, "provider-infiltration"));
    auto& router = a.engine.surfaceRouter2D();
    const double natural =
        router.subsurface().sourceInfiltrationCapacity(0, router.state().depth[0], .7);
    const SWMM_CouplingForcing rows[] = {{sizeof(SWMM_CouplingForcing), 1, -1, 1, 0, 0, 0, nullptr},
                                         {sizeof(SWMM_CouplingForcing), 2, -1, 1, 1, 0, 0, nullptr},
                                         {sizeof(SWMM_CouplingForcing), 3, 0, 1, 1, 0, 0, nullptr},
                                         {sizeof(SWMM_CouplingForcing), 3, 1, 1, 0, 0, 0, nullptr}};
    const SWMM_CouplingFrame f{sizeof(f), 1, "soil", 0, nullptr, 0, nullptr, 4, rows, 0, nullptr};
    ASSERT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&a.engine), &f), SWMM_OK);
    ASSERT_NO_THROW(until(a.engine, 3.5));
    const auto* prescribed = router.state().runtime_forcings.find(3, 0);
    EXPECT_GT(prescribed->receipt.requested, natural * router.mesh().tri_area[0] * 3.5);
    EXPECT_GT(prescribed->receipt.applied, 0);
    EXPECT_LT(prescribed->receipt.applied, prescribed->receipt.requested);
    EXPECT_EQ(prescribed->receipt.water_out, prescribed->receipt.applied);
    EXPECT_EQ(router.state().runtime_forcings.find(3, 1)->receipt.applied, 0);
    double intake = 0;
    for (double v : router.subsurface().state().xacc_from_surface)
        intake += v;
    EXPECT_GE(intake, 0);
    for (double v : router.state().volume)
        EXPECT_GE(v, 0);
    EXPECT_GT(a.stage.meshEvaporation(), 0);
    ASSERT_EQ(a.engine.saveCheckpoint((root() / "provider-infiltration.chk").string()), "");
}
TEST(EngineCheckpoint, RuntimeWaterProfileRejectsChangedReplayIdentityAndPrivateFootprints) {
    SurfaceSession a, b;
    ASSERT_NO_THROW(startProvider(a, "provider-identity-a"));
    ASSERT_NO_THROW(startProvider(b, "provider-identity-b", 0, 0, false, 0, "driver revision 2"));
    ASSERT_EQ(providerFrame(a.engine), SWMM_OK);
    ASSERT_NO_THROW(until(a.engine, 4.2));
    auto good = root() / "provider-identity.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    auto before = root() / "provider-identity-target.chk";
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    const auto initial = bytes(before);
    EXPECT_FALSE(b.engine.loadCheckpoint(good.string()).empty());
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(before), initial);
    EXPECT_FALSE(b.engine.context().runtime_coupling_used);
    SurfaceSession private_sources;
    EXPECT_THROW(private_sources.start("provider-private-footprint", 0, 0, 0, 0, true, true, false,
                                       -1, false, 0, false, "driver revision 1"),
                 std::runtime_error);
    SurfaceSession unqualified;
    ASSERT_NO_THROW(unqualified.start("provider-no-profile"));
    const auto end = unqualified.stage.completedEnd();
    EXPECT_EQ(providerFrame(unqualified.engine), SWMM_ERR_PLUGIN);
    EXPECT_EQ(unqualified.stage.completedEnd(), end);
    EXPECT_FALSE(unqualified.engine.context().runtime_coupling_used);
}
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
TEST(EngineCheckpoint, RuntimeWaterMalformedNestedRecordsPreserveBothOutputsAndAllowRetry) {
    SurfaceSession a;
    ASSERT_NO_THROW(startProvider(a, "provider-atomic-source", 2, 0, true, 2));
    ASSERT_EQ(providerFrame(a.engine), SWMM_OK);
    ASSERT_NO_THROW(until(a.engine, 4.2));
    auto good = root() / "provider-atomic-good.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    for (int mutation = 0; mutation < 9; ++mutation) {
        const auto name = "provider-atomic-target-" + std::to_string(mutation);
        SCOPED_TRACE(name);
        SurfaceSession b;
        ASSERT_NO_THROW(startProvider(b, name, 2, 0, true, 2));
        const auto before = root() / (name + "-before.chk");
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        const auto initial = bytes(before), binary = bytes(root() / (name + ".out")),
                   mesh = bytes(root() / (name + ".h5"));
        const auto bad = root() / (name + "-bad.chk");
        write(bad, changeProviderRecord(bytes(good), mutation));
        EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());
        EXPECT_FALSE(b.engine.context().runtime_coupling_used);
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), initial);
        EXPECT_EQ(bytes(root() / (name + ".out")), binary);
        EXPECT_EQ(bytes(root() / (name + ".h5")), mesh);
        ASSERT_EQ(b.engine.loadCheckpoint(good.string()), "");
        compareMesh(a.engine, b.engine, name + "-retry");
    }
}
TEST(EngineCheckpoint, RuntimeWaterCombinedOutputsContinueReportAndExpiryBoundaries) {
    for (int lid : {0, 1, 2})
        for (int format : {1, 2}) {
            const auto name =
                "provider-combined-" + std::to_string(lid) + "-" + std::to_string(format);
            SCOPED_TRACE(name);
            SurfaceSession a, b;
            ASSERT_NO_THROW(startProvider(a, name, lid, 0, true, format));
            ASSERT_NO_THROW(startProvider(b, name + "-resumed", lid, 0, true, format));
            ASSERT_EQ(providerFrame(a.engine), SWMM_OK);
            ASSERT_NO_THROW(until(a.engine, 4.2));
            const auto cp = root() / (name + ".chk");
            ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
            ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
            compareMesh(a.engine, b.engine, name + "-restored");
            while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
                ASSERT_NO_THROW(step(a.engine));
                ASSERT_NO_THROW(step(b.engine));
            }
            compareMesh(a.engine, b.engine, name + "-final");
            ASSERT_EQ(a.engine.end(), 0);
            ASSERT_EQ(b.engine.end(), 0);
            sameBinary(name, name + "-resumed", 80);
            sameMesh(name, name + "-resumed", 40);
        }
}

#endif
namespace {
// A synchronous stateful adapter: future rates depend on its consumed cursor
// and accumulator. The queue, schema and immutable inputs validate privately.
class TestWaterDriver final : public twoD::RuntimeWaterDriverCheckpoint {
public:
    std::string revision = "mesh water driver revision 1";
    const std::vector<double> schedule{0, 4.2, 7.75, 9.25, 12.125};
    std::vector<double> pending = schedule;
    uint64_t cursor = 0;
    double accumulator = 0;
    bool reject = false, null_plan = false, oversized = false;
    mutable int installs = 0;
    /**
     * @brief Return the test water-driver revision identity by value.
     *
     * Does not capture mutable cursor/state or mutate the driver.
     * @return Owned copy of revision; callers can deliberately alter revision to test incompatible
     *     restores.
     */
    std::string identity() const override { return revision; }
    /**
     * @brief Expose the first pending external driver update time.
     *
     * Does not remove the update or check queue ordering.
     * @return Front of pending in elapsed seconds, or -1 when the queue is empty.
     */
    double nextUpdateSeconds() const noexcept override {
        return pending.empty() ? -1 : pending.front();
    }
    /**
     * @brief Capture owned driver cursor/private state for a session checkpoint.
     *
     * The unnamed elapsed-time argument is intentionally ignored. Serializes cursor, accumulator and
     * pending queue without mutating them; oversized test mode instead returns 8 MiB+1 of invalid
     * filler bytes to exercise the engine payload bound. Capturing itself does not validate clock
     * consistency.
     * @return Snapshot with cursor, next update seconds and owned version-one state bytes.
     */
    Snapshot capture(double) const override {
        detail::CompletedSourceArchive a;
        uint32_t version = 1;
        auto c = cursor;
        auto value = accumulator;
        auto queue = pending;
        a(version, c, value, queue);
        return {cursor, nextUpdateSeconds(),
                oversized ? std::string(8 * 1024 * 1024 + 1, 'x') : a.bytes()};
    }
    /**
     * @brief Validate a saved driver snapshot and prepare a deferred private-state installation.
     *
     * Throws when reject injection is set or schema/cursor/triangular accumulator/exact
     * schedule-tail/finite/clock validation fails. Leaves live driver state untouched until
     * Plan::install. The underlying driver must be mutable, alive and unmoved through installation
     * despite this const preparation interface; plan installation does not reapply engine forcing
     * frames.
     *
     * @param[in] s Borrowed snapshot with version-one private state matching the fixed schedule and
     *     public cursor/next-time fields.
     * @param[in] time Target elapsed engine seconds used to require consumed events not later and
     *     pending events not earlier, within 1e-12 s.
     * @return Owned prepared plan borrowing this driver, or empty when null_plan injection is enabled.
     */
    std::unique_ptr<PreparedRestore> prepareRestore(const Snapshot& s, double time) const override {
        if (reject)
            throw std::runtime_error("Driver rejected its private preparation");
        if (null_plan)
            return {};
        detail::CompletedSourceArchive a(s.state);
        uint32_t version;
        uint64_t c;
        double value;
        std::vector<double> queue;
        a(version, c, value, queue);
        a.finish();
        if (version != 1 || c > schedule.size() || c != s.next_sequence || !std::isfinite(value) ||
            value != double(c * (c + 1)) / 2 ||
            queue != std::vector<double>(schedule.begin() + c, schedule.end()) ||
            s.next_update_seconds != (queue.empty() ? -1 : queue.front()) ||
            (c && schedule[c - 1] > time + 1e-12) ||
            (!queue.empty() && queue.front() < time - 1e-12))
            throw std::runtime_error(
                "Driver schema, cursor, mutable state or pending updates differ");
        /** @cond */
        struct Plan final : PreparedRestore {
            TestWaterDriver& target;
            uint64_t cursor;
            double value;
            std::vector<double> pending;
            /**
             * @brief Own validated restore values while borrowing the destination test driver.
             *
             * Local prepared-plan constructor does not change target state or install runtime coupling
             * prescriptions.
             *
             * @param[in,out] t Mutable target driver that must outlive plan installation/destruction.
             * @param[in] c Validated cursor to install.
             * @param[in] v Validated accumulated event total.
             * @param[in] p Owned-by-value pending schedule moved into plan storage.
             */
            Plan(TestWaterDriver& t, uint64_t c, double v, std::vector<double> p)
                : target(t), cursor(c), value(v), pending(std::move(p)) {}
            /**
             * @brief Publish validated private driver state without throwing.
             *
             * Assigns cursor/accumulator, moves pending queue into the borrowed target and increments installs.
             * Intended for one accepted restore publication; does not modify engine physics or revalidate the
             * prepared values.
             */
            void install() noexcept override {
                target.cursor = cursor;
                target.accumulator = value;
                target.pending = std::move(pending);
                ++target.installs;
            }
        };
        /** @endcond */
        return std::make_unique<Plan>(*const_cast<TestWaterDriver*>(this), c, value,
                                      std::move(queue));
    }
    /**
     * @brief Apply the driver frame due exactly at the accepted engine boundary.
     *
     * Throws if no due event or coupling transaction rejects. First event applies providerFrame; later
     * events apply an adaptive cell-zero source in alternating surface/GW domain with flow
     * 0.003+accumulator*0.0001 m3/s and no expiry. Only after accepted frame increments
     * cursor/triangular accumulator and removes pending front. Driver and engine both mutate; caller
     * serializes dispatch/steps.
     *
     * @param[in,out] e Running compatible engine with elapsed seconds matching pending.front within
     *     1e-12.
     */
    void send(SWMMEngine& e) {
        if (pending.empty() || std::abs(e.context().current_time - pending.front()) > 1e-12)
            throw std::runtime_error("Driver dispatch missed its accepted boundary");
        int rc;
        if (cursor == 0)
            rc = providerFrame(e);
        else {
            const SWMM_CouplingSource row{
                sizeof(row), "adaptive", int(cursor % 2), 0,      .003 + accumulator * .0001, 0,
                0,           0,          nullptr,         nullptr};
            const SWMM_CouplingFrame frame{sizeof(frame), 1, "water", 1, &row,   0,
                                           nullptr,       0, nullptr, 0, nullptr};
            rc = swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&e), &frame);
        }
        if (rc != SWMM_OK)
            throw std::runtime_error("Driver coupling frame failed");
        ++cursor;
        accumulator += cursor;
        pending.erase(pending.begin());
    }
};
/**
 * @brief Advance an engine and dispatch registered driver updates up to a target.
 *
 * Dispatches due updates before stepping while current_time is below target; an update exactly at
 * the final target can remain pending. No iteration guard is used. Missed boundary/coupling/step
 * errors throw with prior accepted mutations retained; target must be within the running
 * simulation.
 *
 * @param[in,out] e Started engine configured to honor driver update boundaries; advanced in place.
 * @param[in,out] d Attached compatible mutable test driver; due frames alter both it and e.
 * @param[in] target Reachable target elapsed seconds; loop tolerance is 1e-12 s.
 */
void drive(SWMMEngine& e, TestWaterDriver& d, double target) {
    while (e.context().current_time < target - 1e-12) {
        if (d.nextUpdateSeconds() >= 0 && d.nextUpdateSeconds() <= e.context().current_time + 1e-12)
            d.send(e);
        else
            step(e);
    }
}
/**
 * @brief Serialize a comparable copy of the test driver public/private snapshot.
 *
 * Does not inspect/validate the captured payload or write a file; capture/allocation exceptions
 * propagate.
 *
 * @param[in] d Borrowed driver whose capture is invoked without mutation.
 * @param[in] time Elapsed seconds passed to capture; this test implementation ignores it.
 * @return Owned bytes containing next sequence, next-update time and opaque state.
 */
std::string driverState(const TestWaterDriver& d, double time) {
    auto s = d.capture(time);
    detail::CompletedSourceArchive a;
    a(s.next_sequence, s.next_update_seconds, s.state);
    return a.bytes();
}
/**
 * @brief Forge an external-driver snapshot or binding inconsistency while preserving enclosing
 * CRCs.
 *
 * Requires the fixture opaque schema and nonempty opaque/pending buffers for pop/back cases. Does
 * not independently validate old CRCs or guarantee every unsupported selector creates an invalid
 * record. Decode/shape/allocation exceptions propagate; no live driver/model state or file is
 * changed.
 *
 * @param[in] original Borrowed known-layout engine archive containing an external water-driver
 *     record.
 * @param[in] mutation 0 identity, 1 record time +0.125 s, 2 water-binding CRC increment, 3 schema
 *     version, 4 public cursor=999, 5 next time before record time, 6 next time +0.5 s, 7 next time
 *     NaN, 8 opaque truncation, 9 last queued event +1 s, 10 whole driver truncation, 11
 *     water-binding CRC replaced by surface-blob CRC; other values only re-encode.
 * @return Owned engine archive with modified driver/state bytes and recomputed enclosing payload
 *     CRC.
 */
std::string changeDriverRecord(const std::string& original, int mutation) {
    using A = detail::CompletedSourceArchive;
    A engine(original);
    std::string em, model, payload;
    uint32_t ev, sum;
    engine(em, ev, model, sum, payload);
    engine.finish();
    const auto driver = nested(payload, "OpenSWMM external water driver").second;
    A record(driver);
    std::string magic, identity, state;
    uint32_t version, endian, water_crc;
    double time;
    record(magic, version, endian, identity, time, water_crc, state, sum);
    record.finish();
    uint64_t cursor;
    double next;
    std::string opaque;
    A body(state);
    body(cursor, next, opaque);
    body.finish();
    if (mutation == 0)
        identity += "changed";
    if (mutation == 1)
        time += .125;
    if (mutation == 2)
        water_crc++;
    if (mutation == 3)
        version++;
    if (mutation == 4)
        cursor = 999;
    if (mutation == 5)
        next = time - .5;
    if (mutation == 6)
        next += .5;
    if (mutation == 7)
        next = std::numeric_limits<double>::quiet_NaN();
    if (mutation == 8)
        opaque.pop_back();
    if (mutation == 9) {
        uint32_t v;
        uint64_t c;
        double value;
        std::vector<double> queue;
        A read(opaque);
        read(v, c, value, queue);
        read.finish();
        queue.back() += 1;
        A out;
        out(v, c, value, queue);
        opaque = out.bytes();
    }
    if (mutation == 11)
        water_crc = crc(engineSurface(original));
    A updated;
    updated(cursor, next, opaque);
    state = updated.bytes();
    sum = crc(state);
    A out;
    out(magic, version, endian, identity, time, water_crc, state, sum);
    auto changed = out.bytes();
    if (mutation == 10)
        changed.pop_back();
    payload = replaceNested(payload, "OpenSWMM external water driver", changed);
    sum = crc(payload);
    A result;
    result(em, ev, model, sum, payload);
    return result.bytes();
}
}

TEST(EngineCheckpoint, ExternalWaterDriverFreshRestartPreservesStateCursorAndFutureUpdates) {
    for (int lid : {0, 1, 2})
        for (int closure : {0, 1, 2}) {
            const auto name =
                "driver-session-" + std::to_string(lid) + "-" + std::to_string(closure);
            SCOPED_TRACE(name);
            TestWaterDriver da, db;
            SurfaceSession a, b;
            ASSERT_NO_THROW(startProvider(a, name + "-a", lid, closure));
            ASSERT_NO_THROW(startProvider(b, name + "-b", lid, closure));
            ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(da), "");
            ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(db), "");
            ASSERT_NO_THROW(drive(a.engine, da, 4.2));
            EXPECT_NEAR(a.engine.context().current_time, 4.2, 1e-12);
            EXPECT_EQ(da.cursor, 1);
            const auto cp = root() / (name + ".chk");
            ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
            ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
            EXPECT_EQ(db.installs, 1);
            EXPECT_EQ(driverState(da, 4.2), driverState(db, 4.2));
            compare(a.engine, b.engine, name + "-restored");
            while (a.engine.context().current_time < 20.125) {
                if (da.nextUpdateSeconds() >= 0 &&
                    da.nextUpdateSeconds() <= a.engine.context().current_time + 1e-12) {
                    ASSERT_NO_THROW(da.send(a.engine));
                    ASSERT_NO_THROW(db.send(b.engine));
                } else {
                    ASSERT_NO_THROW(step(a.engine));
                    ASSERT_NO_THROW(step(b.engine));
                }
                EXPECT_EQ(driverState(da, a.engine.context().current_time),
                          driverState(db, b.engine.context().current_time));
                compare(a.engine, b.engine, name + "-continued");
            }
            EXPECT_EQ(da.cursor, 5);
            EXPECT_EQ(db.nextUpdateSeconds(), -1);
            TestWaterDriver dc;
            SurfaceSession c;
            ASSERT_NO_THROW(startProvider(c, name + "-exhausted", lid, closure));
            ASSERT_EQ(c.engine.attachRuntimeWaterDriverCheckpoint(dc), "");
            ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
            ASSERT_EQ(c.engine.loadCheckpoint(cp.string()), "");
            EXPECT_EQ(dc.nextUpdateSeconds(), -1);
            compare(a.engine, c.engine, name + "-exhausted-restored");
        }
}

TEST(EngineCheckpoint, ExternalWaterDriverDueUpdateStopsBeforeMutationAndAllowsRetry) {
    TestWaterDriver d;
    SurfaceSession s;
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
    ASSERT_NO_THROW(startProvider(s, "driver-due", 2, 0, true, 2));
#else
    ASSERT_NO_THROW(startProvider(s, "driver-due", 2));
#endif
    ASSERT_EQ(s.engine.attachRuntimeWaterDriverCheckpoint(d), "");
    const auto cp = root() / "driver-due.chk";
    ASSERT_EQ(s.engine.saveCheckpoint(cp.string()), "");
    const auto before = bytes(cp);
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
    const auto binary = bytes(root() / "driver-due.out"), mesh = bytes(root() / "driver-due.h5");
#endif
    double elapsed = 123;
    EXPECT_EQ(s.engine.step(&elapsed), SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(elapsed, 123);
    ASSERT_EQ(s.engine.saveCheckpoint(cp.string()), "");
    EXPECT_EQ(bytes(cp), before);
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
    EXPECT_EQ(bytes(root() / "driver-due.out"), binary);
    EXPECT_EQ(bytes(root() / "driver-due.h5"), mesh);
#endif
    EXPECT_EQ(d.cursor, 0);
    ASSERT_NO_THROW(drive(s.engine, d, 4.2));
    EXPECT_NEAR(s.engine.context().current_time, 4.2, 1e-12);
    ASSERT_EQ(s.engine.saveCheckpoint(cp.string()), "");
    const auto due = bytes(cp);
    EXPECT_EQ(s.engine.step(&elapsed), SWMM_ERR_LIFECYCLE);
    ASSERT_EQ(s.engine.saveCheckpoint(cp.string()), "");
    EXPECT_EQ(bytes(cp), due);
    ASSERT_NO_THROW(d.send(s.engine));
    ASSERT_NO_THROW(step(s.engine));
    EXPECT_EQ(d.cursor, 2);
}

#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
TEST(EngineCheckpoint, ExternalWaterDriverNestedGuardsPreserveDriverPhysicsAndBothOutputs) {
    TestWaterDriver da;
    SurfaceSession a;
    ASSERT_NO_THROW(startProvider(a, "driver-guard-source", 2, 0, true, 2));
    ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(da), "");
    ASSERT_NO_THROW(drive(a.engine, da, 5.375));
    const auto good = root() / "driver-guard-good.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    for (int mutation = 0; mutation < 15; ++mutation) {
        SCOPED_TRACE(mutation);
        const auto name = "driver-guard-" + std::to_string(mutation);
        TestWaterDriver db;
        SurfaceSession b;
        ASSERT_NO_THROW(startProvider(b, name, 2, 0, true, 2));
        ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(db), "");
        const auto before = root() / (name + "-before.chk");
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        const auto initial = bytes(before), ds = driverState(db, 0),
                   binary = bytes(root() / (name + ".out")), mesh = bytes(root() / (name + ".h5"));
        auto bad = good;
        if (mutation < 11) {
            bad = root() / (name + "-bad.chk");
            write(bad, changeDriverRecord(bytes(good), mutation));
        }
        if (mutation == 11)
            db.reject = true;
        if (mutation == 12)
            db.null_plan = true;
        if (mutation == 13) {
            bad = root() / (name + "-physical-bad.chk");
            write(bad, changeDriverRecord(changeProviderRecord(bytes(good), 7), 11));
        }
        if (mutation == 14)
            std::filesystem::permissions(root() / (name + ".h5"),
                                         std::filesystem::perms::owner_read);
        EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());
        EXPECT_EQ(db.installs, 0);
        db.reject = db.null_plan = false;
        if (mutation == 14)
            std::filesystem::permissions(root() / (name + ".h5"),
                                         std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_write);
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), initial);
        EXPECT_EQ(driverState(db, 0), ds);
        EXPECT_EQ(bytes(root() / (name + ".out")), binary);
        EXPECT_EQ(bytes(root() / (name + ".h5")), mesh);
        ASSERT_EQ(b.engine.loadCheckpoint(good.string()), "");
        EXPECT_EQ(db.installs, 1);
        EXPECT_EQ(driverState(da, a.engine.context().current_time),
                  driverState(db, b.engine.context().current_time));
        compareMesh(a.engine, b.engine, name + "-retry");
    }
}

TEST(EngineCheckpoint, ExternalWaterDriverCombinedOutputsContinueExactlyAcrossReports) {
    for (int lid : {0, 1, 2})
        for (int format : {1, 2}) {
            const auto name =
                "driver-combined-" + std::to_string(lid) + "-" + std::to_string(format);
            SCOPED_TRACE(name);
            TestWaterDriver da, db;
            SurfaceSession a, b;
            ASSERT_NO_THROW(startProvider(a, name, lid, 0, true, format));
            ASSERT_NO_THROW(startProvider(b, name + "-resumed", lid, 0, true, format));
            ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(da), "");
            ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(db), "");
            ASSERT_NO_THROW(drive(a.engine, da, 9.25));
            const auto cp = root() / (name + ".chk");
            ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
            ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
            ASSERT_NO_THROW(drive(a.engine, da, 13));
            ASSERT_NO_THROW(drive(b.engine, db, 13));
            while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
                ASSERT_NO_THROW(step(a.engine));
                ASSERT_NO_THROW(step(b.engine));
            }
            compareMesh(a.engine, b.engine, name + "-final");
            ASSERT_EQ(a.engine.end(), 0);
            ASSERT_EQ(b.engine.end(), 0);
            sameBinary(name, name + "-resumed", 80);
            sameMesh(name, name + "-resumed", 40);
        }
}

#endif
TEST(EngineCheckpoint, ExternalWaterDriverAttachmentAndCaptureRejectUnsupportedState) {
    TestWaterDriver d;
    SurfaceSession s;
    ASSERT_NO_THROW(startProvider(s, "driver-attach", 2));
    d.revision = "changed inputs";
    EXPECT_FALSE(s.engine.attachRuntimeWaterDriverCheckpoint(d).empty());
    d.revision = "mesh water driver revision 1";
    d.null_plan = true;
    EXPECT_FALSE(s.engine.attachRuntimeWaterDriverCheckpoint(d).empty());
    d.null_plan = false;
    ASSERT_EQ(s.engine.attachRuntimeWaterDriverCheckpoint(d), "");
    EXPECT_FALSE(s.engine.attachRuntimeWaterDriverCheckpoint(d).empty());
    const auto cp = root() / "driver-capture-guard.chk";
    ASSERT_EQ(s.engine.saveCheckpoint(cp.string()), "");
    const auto initial = bytes(cp);
    d.oversized = true;
    EXPECT_FALSE(s.engine.saveCheckpoint(cp.string()).empty());
    EXPECT_EQ(bytes(cp), initial);
    d.oversized = false;
    d.revision += "changed";
    EXPECT_FALSE(s.engine.saveCheckpoint(cp.string()).empty());
    EXPECT_EQ(bytes(cp), initial);
    d.revision = "mesh water driver revision 1";
    ASSERT_NO_THROW(drive(s.engine, d, 1));
    TestWaterDriver late;
    EXPECT_FALSE(s.engine.attachRuntimeWaterDriverCheckpoint(late).empty());
    SurfaceSession unqualified;
    ASSERT_NO_THROW(unqualified.start("driver-unqualified", 0));
    EXPECT_FALSE(unqualified.engine.attachRuntimeWaterDriverCheckpoint(late).empty());
    TestWaterDriver absent;
    SurfaceSession target;
    ASSERT_NO_THROW(startProvider(target, "driver-absent", 2));
    EXPECT_FALSE(target.engine.loadCheckpoint(cp.string()).empty());
}
#endif

#ifdef OPENSWMM_HAS_2D
namespace {
/**
 * @brief Start the runtime-boundary-enabled two-cell checkpoint profile.
 *
 * Uses reversed node mapping, tracked nonspatial checkpointing, fixed runtime driver identity and
 * boundary permission. Does not apply a boundary frame itself; setup errors propagate with partial
 * owned member state retained.
 *
 * @param[in,out] s Fresh owned session with source/stage/engine lifetimes retained.
 * @param[in] name Raw evidence stem below root.
 * @param[in] lid NODE LID mode forwarded to SurfaceSession.
 * @param[in] closure Groundwater closure selector 0..2.
 * @param[in] output Enable binary saved results and averaged reporting together.
 * @param[in] mesh_output Requested mesh-output format selector.
 */
void startBoundary(SurfaceSession& s, const std::string& name, int lid = 0, int closure = 0,
                   bool output = false, int mesh_output = 0) {
    s.start(name, lid, 1, closure, 0, true, false, false, -1, output, mesh_output, output,
            "mesh water driver revision 1", true);
}
/**
 * @brief Find the first boundary edge slot of one fixture mesh cell.
 *
 * Throws runtime_error when none exists. Uses const_cast solely to reach the existing nonconst
 * router getter; helper does not mutate mesh or validate cell bounds.
 *
 * @param[in] e Borrowed live engine with a stable initialized mesh.
 * @param[in] cell Valid zero-based mesh cell index.
 * @return First local edge slot whose neighbour is negative.
 */
int exterior(const SWMMEngine& e, int cell) {
    const auto& m = const_cast<SWMMEngine&>(e).surfaceRouter2D().mesh();
    for (int edge = 0; edge < m.cell_vertex_count(cell); ++edge)
        if (m.cell_neighbour(cell, edge) < 0)
            return edge;
    throw std::runtime_error("Fixture has no exterior edge");
}
/**
 * @brief Apply the fixed two-cell surface/GW runtime-boundary transaction.
 *
 * Builds four stack rows on first exterior slots with datum heads in metres or outward flows in
 * m3/s. API copies rows/provider before return; no input pointers escape. Does not assert
 * acceptance or advance engine time beyond any coupling API pending-physics flush.
 *
 * @param[in,out] e Started compatible engine with at least two cells, each having an exterior edge.
 * @param[in] phase 2 clears authored overrides, 1 uses reversed-flow/head profile, other values use
 *     baseline head/flow profile.
 * @param[in] owner Optional provider name passed to the coupling API; null/empty uses its
 *     legacy-ownership mode.
 * @return swmm_coupling_apply_frame status unchanged.
 */
int boundaryFrame(SWMMEngine& e, int phase = 0, const char* owner = "edge-water") {
    const int a = exterior(e, 0), b = exterior(e, 1);
    const SWMM_CouplingBoundary rows[] = {{sizeof(SWMM_CouplingBoundary), 0, 0, a,
                                           phase == 2   ? 0
                                           : phase == 1 ? 2
                                                        : 1,
                                           phase == 1 ? .5 : .75, 0, nullptr},
                                          {sizeof(SWMM_CouplingBoundary), 0, 1, b,
                                           phase == 2 ? 0 : 2, phase == 1 ? -.005 : .5, 0, nullptr},
                                          {sizeof(SWMM_CouplingBoundary), 1, 0, a,
                                           phase == 2 ? 0 : 1, phase == 1 ? -.5 : 1.55, 0, nullptr},
                                          {sizeof(SWMM_CouplingBoundary), 1, 1, b,
                                           phase == 2 ? 0 : 2, phase == 1 ? -.01 : 2., 0, nullptr}};
    const SWMM_CouplingFrame frame{sizeof(frame), 1, owner,   0, nullptr, 4,
                                   rows,          0, nullptr, 0, nullptr};
    return swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&e), &frame);
}
/**
 * @brief Compare saved continuation state including runtime-boundary records.
 *
 * Delegates physical comparison, then saves both engines again and compares the selected bytes.
 * Requires mesh-output test support for output=true, otherwise throws. Fatal save assertions and
 * decode/IO errors stop the helper; does not step either engine or compare independent result
 * datasets here.
 *
 * @param[in,out] a Reference session at a checkpointable completed boundary.
 * @param[in,out] b Candidate session at the expected same boundary.
 * @param[in] name Raw checkpoint evidence stem below root.
 * @param[in] output Use mesh-output physical comparison and compare nested engine surface records
 *     when true; otherwise compare whole checkpoint bytes.
 */
void compareBoundary(SurfaceSession& a, SurfaceSession& b, const std::string& name,
                     bool output = false) {
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
    if (output)
        compareMesh(a.engine, b.engine, name);
    else
        compare(a.engine, b.engine, name);
#else
    if (output)
        throw std::runtime_error("Mesh output comparisons are unavailable");
    compare(a.engine, b.engine, name);
#endif
    const auto x = root() / (name + "-a.chk"), y = root() / (name + "-b.chk");
    ASSERT_EQ(a.engine.saveCheckpoint(x.string()), "");
    ASSERT_EQ(b.engine.saveCheckpoint(y.string()), "");
    if (output)
        EXPECT_EQ(engineSurface(bytes(x)), engineSurface(bytes(y)));
    else
        EXPECT_EQ(bytes(x), bytes(y));
}
// Rewrite a semantic boundary record, then rebuild every enclosing checksum.
/**
 * @brief Forge runtime-boundary provider/schema/receipt or cross-component layout disagreement.
 *
 * Requires known populated override/receipt/GW arrays, expected CPU boundary index vectors and
 * sufficient raw native-layout offsets; selected shapes are test preconditions rather than fully
 * bounds checked. Uses map/at or marker checks for some but not all accesses, so arbitrary archives
 * are unsupported. Preserves unrelated fields, may intentionally create
 * valid-local/inconsistent-cross-component state, and performs no file/model mutation.
 * Decode/layout/allocation errors propagate.
 *
 * @param[in] original Borrowed known-layout engine/surface/CPU/source/runtime-boundary archive
 *     bytes.
 * @param[in] mutation 0 edge mask, 1 boundary fingerprint byte, 2 old boundary kind, 3 owner with
 *     slash, 4 duplicate override, 5 infinite live head, 6 cumulative flux +0.01, 7 negative
 *     water-out receipt, 8 interior receipt slot, 9 interior GW boundary slot/ID, 10 infinite GW
 *     boundary head, 11 saved stage index, 12 override slot=1000, 13 live stage index, 14 duplicate
 *     receipt, 15 provider version=1, 16 provider time +0.1 s, 17 invalid CPU cell, 18 CPU boundary
 *     NaN flux, 19 invalid wall slot, 20 router/CPU edge mismatch, 21 wrong saved stage name, 22
 *     missing stage name, 23 saved rating index, 24 saved head NaN.
 * @return Owned changed engine archive with all enclosing payload CRCs recomputed.
 */
std::string changeBoundaryRecord(const std::string& original, int mutation) {
    using A = detail::CompletedSourceArchive;
    A engine(original);
    std::string em, model, payload;
    uint32_t ev, sum;
    engine(em, ev, model, sum, payload);
    engine.finish();
    auto surface = engineSurface(original), surface_data = surfacePayload(surface);
    auto cpu = nested(surface_data, "OpenSWMM CPU surface/source boundary").second;
    A ca(cpu);
    std::string cm, cp;
    uint32_t cv, endian, long_bytes;
    ca(cm, cv, endian, long_bytes, cp, sum);
    ca.finish();
    auto source = nested(cp, "OpenSWMM source boundary").second;
    A sa(source);
    std::string sm, source_model, sp;
    uint32_t sv, se, sl, sd;
    sa(sm, sv, se, sl, sd, source_model, sp, sum);
    sa.finish();
    auto provider = nested(sp, "OpenSWMM Runtime Water").second;
    A pa(provider);
    std::string pm, identity, pp, boundary_model;
    uint32_t pv, pe, pi, pr;
    std::vector<bool> cells, edges;
    double time;
    pa(pm, pv, pe, pi, pr, identity, cells, time, edges, boundary_model, pp, sum);
    pa.finish();
    twoD::RuntimeSources s, g;
    twoD::RuntimeForcings f;
    twoD::BoundaryData boundary;
    boundary.resize(int(edges.size()));
    struct Row {
        int slot = 0;
        twoD::BoundaryData::RuntimeOriginal old{};
        std::string owner;
    };
    std::vector<Row> overrides;
    std::vector<std::pair<int, twoD::RuntimeReceipt>> receipts;
    const auto receipt = [](A& a, twoD::RuntimeReceipt& r) {
        a(r.requested, r.applied, r.water_in, r.water_out, r.last_flow, r.last_dt);
    };
    const auto fields = [&](A& a) {
        const auto row = [&](auto& r) {
            a(r.id, r.owner, r.cell, r.flow, r.until, r.enabled, r.boundary_edge, r.boundary_head);
            receipt(a, r.receipt);
        };
        a.sequence(s.rows, row);
        a.sequence(g.rows, row);
        a.sequence(f.rows, [&](auto& r) {
            a(r.channel, r.cell, r.mode, r.rate, r.until, r.enabled, r.owner);
            receipt(a, r.receipt);
        });
        a.fixed(boundary.edge_bc_type, boundary.edge_bed_slope, boundary.edge_bc_head,
                boundary.edge_bc_tseries, boundary.edge_bc_tseries_name, boundary.edge_bc_flow,
                boundary.edge_bc_flow_tseries, boundary.edge_bc_flow_tseries_name,
                boundary.edge_bc_rating_curve, boundary.edge_bc_rating_curve_name,
                boundary.edge_bc_cum_flux);
        a.sequence(overrides, [&](auto& r) {
            a(r.slot);
            auto& v = r.old;
            a(v.type, v.head, v.flow, v.slope, v.stage_ts, v.flow_ts, v.rating, v.stage_name,
              v.flow_name, v.rating_name);
            a(r.owner);
        });
        a.sequence(receipts, [&](auto& r) {
            a(r.first);
            receipt(a, r.second);
        });
    };
    A read(pp);
    fields(read);
    read.finish();
    const int slot = overrides.at(0).slot;
    switch (mutation) {
    case 0:
        edges[slot] = false;
        break;
    case 1:
        boundary_model.back() ^= 1;
        break;
    case 2:
        overrides[0].old.type = int8_t(twoD::BoundaryType::NORMAL_FLOW);
        break;
    case 3:
        overrides[0].owner = "bad/owner";
        break;
    case 4:
        overrides.push_back(overrides[0]);
        break;
    case 5:
        boundary.edge_bc_head[slot] = std::numeric_limits<double>::infinity();
        break;
    case 6:
        boundary.edge_bc_cum_flux[slot] += .01;
        break;
    case 7:
        receipts[0].second.water_out = -1;
        break;
    case 8:
        receipts[0].first = 0;
        break; // cell 0 edge 0 is interior
    case 9:
        g.rows[0].boundary_edge = 0;
        g.rows[0].id = "boundary/0/0";
        break;
    case 10:
        g.rows[0].boundary_head = std::numeric_limits<double>::infinity();
        break;
    case 11:
        overrides[0].old.stage_ts = 0;
        break;
    case 12:
        overrides[0].slot = 1000;
        break;
    case 13:
        boundary.edge_bc_tseries[slot] = 0;
        break;
    case 14:
        receipts.push_back(receipts[0]);
        break;
    case 15:
        pv = 1;
        break;
    case 16:
        time += .1;
        break;
    case 21:
        overrides[0].old.stage_name = "EdgeFlow";
        break;
    case 22:
        overrides[0].old.stage_ts = 0;
        overrides[0].old.stage_name.clear();
        break;
    case 23:
        overrides.at(2).old.rating = 0;
        break;
    case 24:
        overrides[0].old.head = std::numeric_limits<double>::quiet_NaN();
        break;
    }
    A updated;
    fields(updated);
    pp = updated.bytes();
    sum = crc(pp);
    A po;
    po(pm, pv, pe, pi, pr, identity, cells, time, edges, boundary_model, pp, sum);
    sp = replaceNested(sp, "OpenSWMM Runtime Water", po.bytes());
    sum = crc(sp);
    A so;
    so(sm, sv, se, sl, sd, source_model, sp, sum);
    cp = replaceNested(cp, "OpenSWMM source boundary", so.bytes());
    if (mutation >= 17 && mutation <= 19) {
        std::vector<int> bc_cells{0, 1}, bc_slots{1, 4};
        A index;
        index(bc_cells, bc_slots);
        const auto offset = cp.rfind(index.bytes());
        if (offset == std::string::npos)
            throw std::runtime_error("Missing saved CPU boundary layout");
        if (mutation == 17) {
            const int bad_cell = 1000;
            std::memcpy(cp.data() + offset + sizeof(uint64_t), &bad_cell, sizeof(bad_cell));
        }
        if (mutation == 18) {
            const double bad_q = std::numeric_limits<double>::quiet_NaN();
            std::memcpy(cp.data() + offset + index.bytes().size() + sizeof(uint64_t), &bad_q,
                        sizeof(bad_q));
        }
        if (mutation == 19) {
            const int bad_wall =
                1000; // wall slots immediately follow q, accumulators and the CSR pointer
            const auto at = offset + index.bytes().size() +
                            2 * (sizeof(uint64_t) + 2 * sizeof(double)) +
                            (sizeof(uint64_t) + 3 * sizeof(int)) + sizeof(uint64_t);
            std::memcpy(cp.data() + at, &bad_wall, sizeof(bad_wall));
        }
    }
    sum = crc(cp);
    A co;
    co(cm, cv, endian, long_bytes, cp, sum);
    surface_data = replaceNested(surface_data, "OpenSWMM CPU surface/source boundary", co.bytes());
    if (mutation == 20) {
        std::vector<int> slots{1, 4};
        A index;
        index(slots);
        const auto at = surface_data.find(index.bytes());
        if (at == std::string::npos)
            throw std::runtime_error("Missing saved router boundary index");
        const int exterior_wrong = 2;
        std::memcpy(surface_data.data() + at + sizeof(uint64_t), &exterior_wrong,
                    sizeof(exterior_wrong));
    }
    surface = withSurfacePayload(surface, surface_data);
    payload = replaceNested(payload, "OpenSWMM joint DW surface/source boundary", surface);
    sum = crc(payload);
    A result;
    result(em, ev, model, sum, payload);
    return result.bytes();
}
}
TEST(EngineCheckpoint, RuntimeBoundaryActiveRestartPreservesMomentumFallbackAndGrossWater) {
    for (int lid : {0, 1, 2})
        for (int closure : {0, 1, 2}) {
            const auto name =
                "boundary-active-" + std::to_string(lid) + "-" + std::to_string(closure);
            SCOPED_TRACE(name);
            SurfaceSession a, b;
            ASSERT_NO_THROW(startBoundary(a, name + "-a", lid, closure));
            ASSERT_NO_THROW(startBoundary(b, name + "-b", lid, closure));
            ASSERT_EQ(boundaryFrame(a.engine), SWMM_OK);
            ASSERT_NO_THROW(until(a.engine, 3.1));
            const auto file = root() / (name + ".chk");
            ASSERT_EQ(a.engine.saveCheckpoint(file.string()), "");
            ASSERT_EQ(b.engine.loadCheckpoint(file.string()), "");
            compareBoundary(a, b, name + "-restored");
            ASSERT_NO_THROW(until(a.engine, 4.2));
            ASSERT_NO_THROW(until(b.engine, 4.2));
            ASSERT_EQ(boundaryFrame(a.engine, 1), SWMM_OK);
            ASSERT_EQ(boundaryFrame(b.engine, 1), SWMM_OK);
            for (int k = 0; k < 7; ++k) {
                ASSERT_NO_THROW(step(a.engine));
                ASSERT_NO_THROW(step(b.engine));
                compareBoundary(a, b, name + "-continued");
            }
            const auto& mb = a.engine.context().mass_balance_2d;
            EXPECT_GT(mb.boundary_in, 0);
            EXPECT_GT(mb.boundary_out, 0);
            EXPECT_GT(a.engine.surfaceRouter2D().subsurface().state().led_source_in, 0);
            EXPECT_GT(a.engine.surfaceRouter2D().subsurface().state().led_source_out, 0);
            ASSERT_EQ(boundaryFrame(a.engine, 2), SWMM_OK);
            ASSERT_EQ(boundaryFrame(b.engine, 2), SWMM_OK);
            EXPECT_TRUE(a.engine.surfaceRouter2D().boundary().runtime_original.empty());
            EXPECT_TRUE(a.engine.surfaceRouter2D().boundary().runtime_owner.empty());
            compareBoundary(a, b, name + "-cleared");
            for (const auto& r : a.engine.surfaceRouter2D().boundary().receipts)
                EXPECT_NEAR(a.engine.surfaceRouter2D().boundary().edge_bc_cum_flux[r.first],
                            -r.second.applied, 1e-12);
        }
}
TEST(EngineCheckpoint, RuntimeBoundaryClearedHistoryRestoresPendingWorkAndNewOwnership) {
    SurfaceSession a, b;
    ASSERT_NO_THROW(startBoundary(a, "boundary-idle-a", 2));
    ASSERT_NO_THROW(startBoundary(b, "boundary-idle-b", 2));
    ASSERT_EQ(boundaryFrame(a.engine), SWMM_OK);
    ASSERT_NO_THROW(until(a.engine, 2.1));
    ASSERT_EQ(boundaryFrame(a.engine, 2), SWMM_OK);
    ASSERT_NO_THROW(step(a.engine));
    ASSERT_LT(a.stage.completedEnd(), a.engine.context().current_time);
    const auto good = root() / "boundary-idle.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    ASSERT_EQ(b.engine.loadCheckpoint(good.string()), "");
    compareBoundary(a, b, "boundary-idle-restored");
    const auto old = a.engine.context().mass_balance_2d.boundary_in;
    ASSERT_EQ(boundaryFrame(a.engine, 1, "replacement"), SWMM_OK);
    ASSERT_EQ(boundaryFrame(b.engine, 1, "replacement"), SWMM_OK);
    EXPECT_EQ(a.engine.context().mass_balance_2d.boundary_in, old);
    ASSERT_NO_THROW(until(a.engine, 10));
    ASSERT_NO_THROW(until(b.engine, 10));
    compareBoundary(a, b, "boundary-new-owner");
    EXPECT_EQ(b.engine.surfaceRouter2D().boundary().runtime_owner.begin()->second, "replacement");
}
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
TEST(EngineCheckpoint, RuntimeBoundaryInvalidNestedStatePreservesPhysicsBothOutputsAndAllowsRetry) {
    SurfaceSession a;
    ASSERT_NO_THROW(startBoundary(a, "boundary-guard-a", 2, 0, true, 2));
    ASSERT_EQ(boundaryFrame(a.engine), SWMM_OK);
    ASSERT_NO_THROW(until(a.engine, 8.4));
    const auto good = root() / "boundary-guard.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    for (int mutation = 0; mutation < 21; ++mutation) {
        const auto name = "boundary-guard-" + std::to_string(mutation);
        SCOPED_TRACE(name);
        SurfaceSession b;
        ASSERT_NO_THROW(startBoundary(b, name, 2, 0, true, 2));
        const auto before = root() / (name + "-before.chk"), bad = root() / (name + "-bad.chk");
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        const auto saved = bytes(before), binary = bytes(root() / (name + ".out")),
                   mesh = bytes(root() / (name + ".h5"));
        write(bad, changeBoundaryRecord(bytes(good), mutation));
        EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), saved);
        EXPECT_EQ(bytes(root() / (name + ".out")), binary);
        EXPECT_EQ(bytes(root() / (name + ".h5")), mesh);
        noOutputStaging(name);
        ASSERT_EQ(b.engine.loadCheckpoint(good.string()), "");
        compareMesh(a.engine, b.engine, name + "-valid-retry");
    }
}
#endif
TEST(EngineCheckpoint, RuntimeBoundaryFramesRejectOwnershipGeometryAndQualityBeforeBarrier) {
    SurfaceSession a;
    ASSERT_NO_THROW(startBoundary(a, "boundary-api-guard", 2));
    ASSERT_EQ(boundaryFrame(a.engine), SWMM_OK);
    ASSERT_NO_THROW(until(a.engine, 2.1));
    const auto file = root() / "boundary-api-before.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(file.string()), "");
    const auto saved = bytes(file);
    EXPECT_EQ(boundaryFrame(a.engine, 1, "other"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(boundaryFrame(a.engine, 2, "other"), SWMM_ERR_BADPARAM);
    for (int mutation = 0; mutation < 4; ++mutation) {
        const double concentration = 1;
        SWMM_CouplingBoundary row{sizeof(row), 0, 0, exterior(a.engine, 0), 1, .75, 0, nullptr};
        if (mutation == 0)
            row.edge = 0;
        if (mutation == 1)
            row.edge = 3;
        if (mutation == 2)
            row.value = std::numeric_limits<double>::infinity();
        if (mutation == 3) {
            row.species_count = 1;
            row.concentrations = &concentration;
        }
        SWMM_CouplingFrame frame{sizeof(frame), 1, "edge-water", 0, nullptr, 1,
                                 &row,          0, nullptr,      0, nullptr};
        EXPECT_NE(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&a.engine), &frame),
                  SWMM_OK);
        ASSERT_EQ(a.engine.saveCheckpoint(file.string()), "");
        EXPECT_EQ(bytes(file), saved);
    }
    auto& boundary = a.engine.surfaceRouter2D().boundary();
    const auto slot = boundary.runtime_original.begin()->first;
    const auto original = boundary.runtime_original.at(slot);
    boundary.runtime_original.at(slot).stage_ts = 0;
    EXPECT_FALSE(a.engine.saveCheckpoint(file.string()).empty());
    EXPECT_EQ(bytes(file), saved);
    boundary.runtime_original.at(slot) = original;
    ASSERT_EQ(a.engine.saveCheckpoint(file.string()), "");
    EXPECT_EQ(bytes(file), saved);
    SurfaceSession no_identity;
    EXPECT_THROW(no_identity.start("boundary-needs-water", 2, 1, 0, 0, true, false, false, -1,
                                   false, 0, false, "", true),
                 std::runtime_error);
    SurfaceSession old;
    ASSERT_NO_THROW(startProvider(old, "boundary-old-profile", 2));
    EXPECT_EQ(boundaryFrame(old.engine), SWMM_ERR_PLUGIN);
    EXPECT_FALSE(old.engine.loadCheckpoint(file.string()).empty());
}
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
TEST(EngineCheckpoint, RuntimeBoundaryCombinedReportsContinueAcrossClearAndReacquisition) {
    for (int format : {1, 2})
        for (int lid : {0, 1, 2}) {
            const auto name =
                "boundary-output-" + std::to_string(format) + "-" + std::to_string(lid);
            SCOPED_TRACE(name);
            SurfaceSession a, b;
            ASSERT_NO_THROW(startBoundary(a, name, lid, 0, true, format));
            ASSERT_NO_THROW(startBoundary(b, name + "-resumed", lid, 0, true, format));
            ASSERT_EQ(boundaryFrame(a.engine), SWMM_OK);
            ASSERT_NO_THROW(until(a.engine, 9.1));
            const auto checkpoint = root() / (name + ".chk");
            ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
            ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
            ASSERT_EQ(boundaryFrame(a.engine, 2), SWMM_OK);
            ASSERT_EQ(boundaryFrame(b.engine, 2), SWMM_OK);
            ASSERT_NO_THROW(until(a.engine, 13.1));
            ASSERT_NO_THROW(until(b.engine, 13.1));
            ASSERT_EQ(boundaryFrame(a.engine, 1, "new-owner"), SWMM_OK);
            ASSERT_EQ(boundaryFrame(b.engine, 1, "new-owner"), SWMM_OK);
            while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
                ASSERT_NO_THROW(step(a.engine));
                ASSERT_NO_THROW(step(b.engine));
            }
            compareMesh(a.engine, b.engine, name + "-final");
            ASSERT_EQ(a.engine.end(), 0);
            ASSERT_EQ(b.engine.end(), 0);
            sameBinary(name, name + "-resumed", 80);
            sameMesh(name, name + "-resumed", 40);
        }
}
TEST(EngineCheckpoint, RuntimeBoundaryDriverDueUpdateAndPublicationFailureRemainRetryable) {
    TestWaterDriver da, db;
    SurfaceSession a, b;
    ASSERT_NO_THROW(startBoundary(a, "boundary-driver-a", 2, 0, true, 2));
    ASSERT_NO_THROW(startBoundary(b, "boundary-driver-b", 2, 0, true, 2));
    ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(da), "");
    ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(db), "");
    ASSERT_EQ(boundaryFrame(a.engine), SWMM_OK);
    ASSERT_NO_THROW(drive(a.engine, da, 4.2));
    const auto good = root() / "boundary-driver.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()), "");
    const auto before = root() / "boundary-driver-before.chk";
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    const auto saved = bytes(before), state = driverState(db, 0);
    const auto binary = root() / "boundary-driver-b.out", mesh = root() / "boundary-driver-b.h5";
    const auto bh = bytes(binary), mh = bytes(mesh);
    std::filesystem::permissions(mesh, std::filesystem::perms::owner_read);
    EXPECT_FALSE(b.engine.loadCheckpoint(good.string()).empty());
    EXPECT_EQ(driverState(db, 0), state);
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(before), saved);
    EXPECT_EQ(bytes(binary), bh);
    EXPECT_EQ(bytes(mesh), mh);
    noOutputStaging("boundary-driver-b");
    std::filesystem::permissions(mesh, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write);
    ASSERT_EQ(b.engine.loadCheckpoint(good.string()), "");
    compareBoundary(a, b, "boundary-driver-restored", true);
    ASSERT_NO_THROW(drive(a.engine, da, 15));
    ASSERT_NO_THROW(drive(b.engine, db, 15));
    compareBoundary(a, b, "boundary-driver-continued", true);
}
#endif
#endif

#ifdef OPENSWMM_HAS_2D
namespace {
/**
 * @brief Start an authored-edge session with optional runtime-boundary permission.
 *
 * Uses fixed runtime driver identity and nonspatial source profile; authored rows remain active
 * until an accepted override/clear frame. Does not apply runtime forcing here; downstream
 * validation/setup errors propagate.
 *
 * @param[in,out] s Fresh owned session.
 * @param[in] name Raw evidence stem below root.
 * @param[in] authored Authored boundary profile selector forwarded to SurfaceSession, normally
 *     1..8.
 * @param[in] lid NODE LID mode.
 * @param[in] closure Groundwater closure selector 0..2.
 * @param[in] output Enable binary output and averaged reporting together.
 * @param[in] precision Mesh-output format selector.
 * @param[in] variant Authored stage signal variant selector.
 * @param[in] track Enable checkpoint attachment when true.
 * @param[in] profile Enable runtime-boundary authorization when true.
 */
void startAuthoredBoundary(SurfaceSession& s, const std::string& name, int authored = 7,
                           int lid = 2, int closure = 0, bool output = false, int precision = 0,
                           int variant = 0, bool track = true, bool profile = true) {
    s.start(name, lid, 1, closure, 0, track, false, false, -1, output, precision, output,
            "mesh water driver revision 1", profile, authored, variant);
}
/**
 * @brief Apply or clear runtime overrides on every exterior surface edge and one GW edge per cell.
 *
 * Uses owned temporary rows and stack frame; public API copies them before return. Enumerates all
 * cells/edges without changing topology, does not assert success and relies on coupling API atomic
 * validation/flush semantics.
 *
 * @param[in,out] e Started compatible engine with valid mesh and at least one exterior edge per
 *     cell.
 * @param[in] phase 2 clears overrides, 1 sets surface outward flow -0.002 m3/s, other values set
 *     surface datum stage 0.62 m; GW stage is 1.55 m except on clear.
 * @param[in] owner Optional provider identity passed through the coupling API.
 * @return Coupling apply-frame status unchanged.
 */
int authoredBoundaryFrame(SWMMEngine& e, int phase = 0, const char* owner = "authored-edge") {
    std::vector<SWMM_CouplingBoundary> rows;
    const auto& mesh = e.surfaceRouter2D().mesh();
    for (int cell = 0; cell < mesh.n_cells(); ++cell)
        for (int edge = 0; edge < mesh.cell_vertex_count(cell); ++edge) {
            if (mesh.cell_neighbour(cell, edge) >= 0)
                continue;
            const int kind = phase == 2 ? 0 : (phase == 1 ? 2 : 1);
            rows.push_back({sizeof(SWMM_CouplingBoundary), 0, cell, edge, kind,
                            phase == 1 ? -.002 : .62, 0, nullptr});
        }
    for (int cell = 0; cell < mesh.n_cells(); ++cell)
        rows.push_back({sizeof(SWMM_CouplingBoundary), 1, cell, exterior(e, cell),
                        phase == 2 ? 0 : 1, 1.55, 0, nullptr});
    SWMM_CouplingFrame frame{sizeof(frame), 1, owner,   0, nullptr, int(rows.size()),
                             rows.data(),   0, nullptr, 0, nullptr};
    return swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&e), &frame);
}
/**
 * @brief Flush pending surface/source batch work at the current fixture boundary.
 *
 * Calls SurfaceRouter2D::flushPendingBatch directly; pending physics/ledger state may mutate and
 * errors propagate. Does not explicitly advance the engine context clock or end output.
 *
 * @param[in,out] e Live initialized engine with compatible router/context ownership.
 */
void flushBoundary(SWMMEngine& e) {
    e.surfaceRouter2D().flushPendingBatch(e.context());
}
}
TEST(EngineCheckpoint, AuthoredBoundaryOnlyContinuationKeepsPhysicalFluxSeparateFromProviderUse) {
    for (int authored = 1; authored <= 8; ++authored)
        for (int lid : {0, 1, 2})
            for (int closure : {0, 1, 2}) {
                const auto name = "authored-only-" + std::to_string(authored) + "-" +
                                  std::to_string(lid) + "-" + std::to_string(closure);
                SCOPED_TRACE(name);
                SurfaceSession a, b;
                ASSERT_NO_THROW(startAuthoredBoundary(a, name + "-a", authored, lid, closure));
                ASSERT_NO_THROW(startAuthoredBoundary(b, name + "-b", authored, lid, closure));
                ASSERT_NO_THROW(until(a.engine, 9.1));
                EXPECT_FALSE(a.engine.context().runtime_coupling_used);
                const auto cp = root() / (name + ".chk");
                ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
                ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
                compareBoundary(a, b, name + "-restored");
                ASSERT_NO_THROW(until(a.engine, 23.1));
                ASSERT_NO_THROW(until(b.engine, 23.1));
                ASSERT_NO_THROW(flushBoundary(a.engine));
                ASSERT_NO_THROW(flushBoundary(b.engine));
                compareBoundary(a, b, name + "-continued");
                EXPECT_FALSE(b.engine.context().runtime_coupling_used);
                const auto& mb = a.engine.context().mass_balance_2d;
                EXPECT_GT(mb.boundary_in + mb.boundary_out, 0);
            }
}
TEST(EngineCheckpoint, AuthoredBoundaryActiveOverridesClearToCurrentSignalsAndReacquire) {
    for (int authored : {7, 8})
        for (int lid : {0, 1, 2})
            for (int closure : {0, 1, 2}) {
                const auto name = "authored-active-" + std::to_string(authored) + "-" +
                                  std::to_string(lid) + "-" + std::to_string(closure);
                SCOPED_TRACE(name);
                SurfaceSession a, b;
                ASSERT_NO_THROW(startAuthoredBoundary(a, name + "-a", authored, lid, closure));
                ASSERT_NO_THROW(startAuthoredBoundary(b, name + "-b", authored, lid, closure));
                ASSERT_NO_THROW(until(a.engine, 4.1));
                ASSERT_EQ(authoredBoundaryFrame(a.engine), SWMM_OK);
                ASSERT_NO_THROW(until(a.engine, 9.1));
                const auto cp = root() / (name + ".chk");
                ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
                ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
                compareBoundary(a, b, name + "-restored");
                ASSERT_EQ(authoredBoundaryFrame(a.engine, 1), SWMM_OK);
                ASSERT_EQ(authoredBoundaryFrame(b.engine, 1), SWMM_OK);
                ASSERT_NO_THROW(until(a.engine, 12.1));
                ASSERT_NO_THROW(until(b.engine, 12.1));
                ASSERT_EQ(authoredBoundaryFrame(a.engine, 2), SWMM_OK);
                ASSERT_EQ(authoredBoundaryFrame(b.engine, 2), SWMM_OK);
                compareBoundary(a, b, name + "-clear");
                auto& r = a.engine.surfaceRouter2D();
                const auto& mesh = r.mesh();
                const int stage = twoD::MeshData::slot(0, 1), flow = twoD::MeshData::slot(0, 2),
                          rating = twoD::MeshData::slot(1, 0);
                const double time = a.stage.completedEnd(), head = r.state().head[1];
                auto st = a.engine.context().tables.tables.at(
                    a.engine.context().find_timeseries("EdgeStage"));
                auto ft = a.engine.context().tables.tables.at(
                    a.engine.context().find_timeseries("EdgeFlow"));
                const auto curve = a.engine.context().tables.tables.at(
                    a.engine.context().find_curve("EdgeRating"));
                ASSERT_NO_THROW(step(a.engine));
                ASSERT_NO_THROW(step(b.engine));
                ASSERT_NO_THROW(flushBoundary(a.engine));
                ASSERT_NO_THROW(flushBoundary(b.engine));
                const double at = datetime::addSeconds(a.engine.context().options.start_date, time);
                EXPECT_DOUBLE_EQ(r.boundary().edge_bc_head[stage],
                                 .3048 * table_lookup_cursor(st, at));
                EXPECT_DOUBLE_EQ(r.boundary().edge_bc_flow[flow],
                                 .028316846592 * table_lookup_cursor(ft, at));
                EXPECT_DOUBLE_EQ(r.boundary().edge_bc_flow[rating],
                                 .028316846592 * table_lookupEx(curve, head / .3048));
                EXPECT_TRUE(r.boundary().runtime_original.empty());
                EXPECT_TRUE(r.boundary().runtime_owner.empty());
                compareBoundary(a, b, name + "-fallback");
                ASSERT_EQ(authoredBoundaryFrame(a.engine, 0, "new-edge-owner"), SWMM_OK);
                ASSERT_EQ(authoredBoundaryFrame(b.engine, 0, "new-edge-owner"), SWMM_OK);
                ASSERT_NO_THROW(until(a.engine, 25.1));
                ASSERT_NO_THROW(until(b.engine, 25.1));
                compareBoundary(a, b, name + "-reacquired");
            }
}
TEST(EngineCheckpoint, AuthoredBoundaryDeferredSignalsRestoreBeforeFirstEvaluationAndAfterClear) {
    SurfaceSession a, b, c;
    ASSERT_NO_THROW(startAuthoredBoundary(a, "authored-deferred-a"));
    ASSERT_NO_THROW(startAuthoredBoundary(b, "authored-deferred-b"));
    ASSERT_NO_THROW(startAuthoredBoundary(c, "authored-deferred-c"));
    ASSERT_EQ(authoredBoundaryFrame(a.engine), SWMM_OK);
    const auto cp = root() / "authored-deferred.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
    compareBoundary(a, b, "authored-deferred-restore");
    ASSERT_NO_THROW(until(a.engine, 8.1));
    ASSERT_NO_THROW(until(b.engine, 8.1));
    ASSERT_EQ(authoredBoundaryFrame(a.engine, 2), SWMM_OK);
    ASSERT_EQ(authoredBoundaryFrame(b.engine, 2), SWMM_OK);
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    ASSERT_EQ(c.engine.loadCheckpoint(cp.string()), "");
    ASSERT_NO_THROW(until(a.engine, 24.1));
    ASSERT_NO_THROW(until(b.engine, 24.1));
    ASSERT_NO_THROW(until(c.engine, 24.1));
    compareBoundary(a, b, "authored-deferred-b-final");
    compareBoundary(a, c, "authored-deferred-c-final");
}
TEST(EngineCheckpoint, AuthoredBoundarySignalBindingsAndOldProfilesRejectChangedTargets) {
    SurfaceSession a;
    ASSERT_NO_THROW(startAuthoredBoundary(a, "authored-bind-source"));
    ASSERT_NO_THROW(until(a.engine, 5.1));
    ASSERT_EQ(authoredBoundaryFrame(a.engine), SWMM_OK);
    const auto cp = root() / "authored-bind.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    SurfaceSession changed;
    ASSERT_NO_THROW(startAuthoredBoundary(changed, "authored-bind-changed", 7, 2, 0, false, 0, 1));
    const auto before = root() / "authored-bind-before.chk";
    ASSERT_EQ(changed.engine.saveCheckpoint(before.string()), "");
    const auto initial = bytes(before);
    EXPECT_FALSE(changed.engine.loadCheckpoint(cp.string()).empty());
    ASSERT_EQ(changed.engine.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(before), initial);
    SurfaceSession old;
    EXPECT_THROW(startAuthoredBoundary(old, "authored-bind-old", 7, 2, 0, false, 0, 0, true, false),
                 std::runtime_error);
    auto& boundary = a.engine.surfaceRouter2D().boundary();
    const auto slot = boundary.runtime_original.begin()->first;
    const auto descriptor = boundary.runtime_original.at(slot);
    const auto saved = bytes(cp);
    boundary.runtime_original.at(slot).stage_name = "EdgeFlow";
    EXPECT_FALSE(a.engine.saveCheckpoint(cp.string()).empty());
    EXPECT_EQ(bytes(cp), saved);
    boundary.runtime_original.at(slot) = descriptor;
    auto& table =
        a.engine.context().tables.tables.at(a.engine.context().find_timeseries("EdgeStage"));
    const auto y = table.y;
    table.y[0] += .1;
    EXPECT_FALSE(a.engine.saveCheckpoint(cp.string()).empty());
    EXPECT_EQ(bytes(cp), saved);
    table.y = y;
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    EXPECT_EQ(bytes(cp), saved);
}
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
TEST(EngineCheckpoint, AuthoredBoundaryNestedSignalDamageAndPublicationFailurePreserveBothOutputs) {
    SurfaceSession a;
    ASSERT_NO_THROW(startAuthoredBoundary(a, "authored-atomic-source", 7, 2, 0, true, 2));
    ASSERT_NO_THROW(until(a.engine, 4.1));
    ASSERT_EQ(authoredBoundaryFrame(a.engine), SWMM_OK);
    ASSERT_NO_THROW(until(a.engine, 9.1));
    const auto cp = root() / "authored-atomic.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    for (int mutation :
         {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 21, 22, 23, 24, 25}) {
        const auto name = "authored-mutant-" + std::to_string(mutation);
        SCOPED_TRACE(name);
        SurfaceSession b;
        ASSERT_NO_THROW(startAuthoredBoundary(b, name, 7, 2, 0, true, 2));
        const auto before = root() / (name + "-before.chk");
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        const auto initial = bytes(before);
        const auto binary = bytes(root() / (name + ".out")), mesh = bytes(root() / (name + ".h5"));
        auto bad = cp;
        if (mutation < 25) {
            bad = root() / (name + "-bad.chk");
            write(bad, changeBoundaryRecord(bytes(cp), mutation));
        } else
            std::filesystem::permissions(root() / (name + ".h5"),
                                         std::filesystem::perms::owner_read);
        EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());
        if (mutation == 25)
            std::filesystem::permissions(root() / (name + ".h5"),
                                         std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_write);
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), initial);
        EXPECT_EQ(bytes(root() / (name + ".out")), binary);
        EXPECT_EQ(bytes(root() / (name + ".h5")), mesh);
        ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
        compareBoundary(a, b, name + "-retry", true);
    }
}
TEST(EngineCheckpoint, AuthoredBoundaryCombinedReportsContinueAcrossCurrentTimeFallback) {
    for (int format : {1, 2})
        for (int lid : {0, 1, 2}) {
            const auto name =
                "authored-output-" + std::to_string(format) + "-" + std::to_string(lid);
            SCOPED_TRACE(name);
            SurfaceSession a, b;
            ASSERT_NO_THROW(startAuthoredBoundary(a, name, 8, lid, 0, true, format));
            ASSERT_NO_THROW(startAuthoredBoundary(b, name + "-resumed", 8, lid, 0, true, format));
            ASSERT_NO_THROW(until(a.engine, 4.1));
            ASSERT_EQ(authoredBoundaryFrame(a.engine), SWMM_OK);
            ASSERT_NO_THROW(until(a.engine, 9.1));
            const auto cp = root() / (name + ".chk");
            ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
            ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
            ASSERT_EQ(authoredBoundaryFrame(a.engine, 2), SWMM_OK);
            ASSERT_EQ(authoredBoundaryFrame(b.engine, 2), SWMM_OK);
            ASSERT_NO_THROW(until(a.engine, 23.1));
            ASSERT_NO_THROW(until(b.engine, 23.1));
            ASSERT_EQ(authoredBoundaryFrame(a.engine, 1, "resumed-owner"), SWMM_OK);
            ASSERT_EQ(authoredBoundaryFrame(b.engine, 1, "resumed-owner"), SWMM_OK);
            while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
                ASSERT_NO_THROW(step(a.engine));
                ASSERT_NO_THROW(step(b.engine));
            }
            compareBoundary(a, b, name + "-final", true);
            ASSERT_EQ(a.engine.end(), 0);
            ASSERT_EQ(b.engine.end(), 0);
            sameBinary(name, name + "-resumed", 80);
            sameMesh(name, name + "-resumed", 40);
        }
}
TEST(EngineCheckpoint, AuthoredBoundaryDueDriverRestoreKeepsSignalsAndPublicationRetryAtomic) {
    TestWaterDriver da, db;
    SurfaceSession a, b;
    ASSERT_NO_THROW(startAuthoredBoundary(a, "authored-driver-a", 7, 2, 0, true, 2));
    ASSERT_NO_THROW(startAuthoredBoundary(b, "authored-driver-b", 7, 2, 0, true, 2));
    ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(da), "");
    ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(db), "");
    ASSERT_EQ(authoredBoundaryFrame(a.engine), SWMM_OK);
    ASSERT_NO_THROW(drive(a.engine, da, 4.2));
    EXPECT_DOUBLE_EQ(da.nextUpdateSeconds(), a.engine.context().current_time);
    const auto cp = root() / "authored-driver.chk", before = root() / "authored-driver-before.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    const auto initial = bytes(before), driver = driverState(db, 0);
    const auto binary = root() / "authored-driver-b.out", mesh = root() / "authored-driver-b.h5";
    const auto bh = bytes(binary), mh = bytes(mesh);
    std::filesystem::permissions(mesh, std::filesystem::perms::owner_read);
    EXPECT_FALSE(b.engine.loadCheckpoint(cp.string()).empty());
    EXPECT_EQ(driverState(db, 0), driver);
    EXPECT_EQ(db.installs, 0);
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(before), initial);
    EXPECT_EQ(bytes(binary), bh);
    EXPECT_EQ(bytes(mesh), mh);
    std::filesystem::permissions(mesh, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write);
    ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
    EXPECT_EQ(db.installs, 1);
    compareBoundary(a, b, "authored-driver-restored", true);
    ASSERT_EQ(authoredBoundaryFrame(a.engine, 2), SWMM_OK);
    ASSERT_EQ(authoredBoundaryFrame(b.engine, 2), SWMM_OK);
    ASSERT_NO_THROW(drive(a.engine, da, 25));
    ASSERT_NO_THROW(drive(b.engine, db, 25));
    EXPECT_EQ(driverState(da, a.engine.context().current_time),
              driverState(db, b.engine.context().current_time));
    compareBoundary(a, b, "authored-driver-final", true);
}

#endif
#endif

#ifdef OPENSWMM_HAS_2D
namespace {
/**
 * @brief Start a reviewed spatial catchment-forcing checkpoint profile.
 *
 * Uses tracked spatial ownership, reversed node mapping and catchment runtime forcing. Does not
 * apply an override frame itself; SurfaceSession setup failures propagate with partial state
 * retained.
 *
 * @param[in,out] s Fresh session retaining owned source/stage/engine lifetimes.
 * @param[in] name Raw evidence stem below root.
 * @param[in] lid NODE LID mode.
 * @param[in] closure Groundwater closure selector 0..2.
 * @param[in] output Enable saved binary output and averaged reporting together.
 * @param[in] format Mesh-output format selector forwarded to SurfaceSession.
 * @param[in] identity Borrowed runtime driver identity copied during initialization.
 * @param[in] boundaries Enable runtime boundaries and mixed authored boundary profile seven when
 *     true.
 */
void startCatchmentProvider(SurfaceSession& s, const std::string& name, int lid = 0,
                            int closure = 0, bool output = false, int format = 0,
                            const std::string& identity = "mesh water driver revision 1",
                            bool boundaries = false) {
    s.start(name, lid, 1, closure, 0, true, true, false, -1, output, format, output, identity,
            boundaries, boundaries ? 7 : 0, 0, true);
}
/**
 * @brief Compare physical continuation and selected catchment/source water state.
 *
 * Compares checkpoint physical bytes, surface volume, unsaturated storage, source subarea depths
 * and source-zero/one rainfall/ET/infiltration ledgers. Nonfatally checks source-zero balance
 * residual near zero inside both loop iterations, as implemented; does not claim to check
 * source-one residual. Requires consistent two-source arrays and does not advance models.
 *
 * @param[in,out] a Reference attached spatial session.
 * @param[in,out] b Candidate session at the expected same completed boundary.
 * @param[in] name Raw checkpoint artifact stem below root.
 * @param[in] output Use mesh-output physical comparison when true, requiring compiled mesh-output
 *     test support.
 */
void compareCatchmentProvider(SurfaceSession& a, SurfaceSession& b, const std::string& name,
                              bool output = false) {
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
    if (output)
        compareMesh(a.engine, b.engine, name);
    else
        compare(a.engine, b.engine, name);
#else
    if (output)
        throw std::runtime_error("Mesh output comparisons are unavailable");
    compare(a.engine, b.engine, name);
#endif
    EXPECT_EQ(a.engine.surfaceRouter2D().state().volume, b.engine.surfaceRouter2D().state().volume);
    EXPECT_EQ(a.engine.surfaceRouter2D().subsurface().state().hu,
              b.engine.surfaceRouter2D().subsurface().state().hu);
    EXPECT_EQ(a.sources.runoff().soa().depth_perv, b.sources.runoff().soa().depth_perv);
    EXPECT_EQ(a.sources.runoff().soa().depth_imperv0, b.sources.runoff().soa().depth_imperv0);
    for (int sc : {0, 1}) {
        const auto& x = a.sources.ledgers()[sc];
        const auto& y = b.sources.ledgers()[sc];
        EXPECT_DOUBLE_EQ(x.rain, y.rain);
        EXPECT_DOUBLE_EQ(x.evaporation, y.evaporation);
        EXPECT_DOUBLE_EQ(x.spatial_infiltration, y.spatial_infiltration);
        EXPECT_DOUBLE_EQ(x.outside_infiltration, y.outside_infiltration);
        EXPECT_NEAR(a.sources.balanceResidual(0), 0, 1e-6);
    }
}
}
TEST(EngineCheckpoint, CatchmentProviderSessionRestoresPrivateStoresAndNodeLidProfiles) {
    for (int lid : {0, 1, 2})
        for (int closure : {0, 1, 2}) {
            const auto name =
                "catchment-provider-" + std::to_string(lid) + "-" + std::to_string(closure);
            SCOPED_TRACE(name);
            SurfaceSession a, b;
            ASSERT_NO_THROW(startCatchmentProvider(a, name + "-a", lid, closure));
            ASSERT_NO_THROW(startCatchmentProvider(b, name + "-b", lid, closure));
            ASSERT_TRUE(a.stage.runtimeCatchmentForcingEnabled());
            ASSERT_FALSE(a.stage.meshOnly());
            ASSERT_EQ(providerFrame(a.engine), SWMM_OK);
            ASSERT_NO_THROW(until(a.engine, 4.2));
            const auto cp = root() / (name + ".chk");
            ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
            ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
            compareCatchmentProvider(a, b, name + "-restored");
            ASSERT_NO_THROW(until(a.engine, 13.1));
            ASSERT_NO_THROW(until(b.engine, 13.1));
            compareCatchmentProvider(a, b, name + "-expired");
            const SWMM_CouplingForcing rain{sizeof(rain), 1, 0, 2, .003, 20, 0, nullptr};
            const SWMM_CouplingFrame frame{sizeof(frame), 1, "water", 0, nullptr, 0,
                                           nullptr,       1, &rain,   0, nullptr};
            ASSERT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&a.engine), &frame),
                      SWMM_OK);
            ASSERT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&b.engine), &frame),
                      SWMM_OK);
            ASSERT_NO_THROW(until(a.engine, 23.1));
            ASSERT_NO_THROW(until(b.engine, 23.1));
            compareCatchmentProvider(a, b, name + "-continued");
        }
}
TEST(EngineCheckpoint, CatchmentProviderIdentityAndFutureFrameRefusalPreserveAcceptedSources) {
    SurfaceSession a, b;
    ASSERT_NO_THROW(startCatchmentProvider(a, "catchment-bind-a", 2));
    ASSERT_NO_THROW(
        startCatchmentProvider(b, "catchment-bind-b", 2, 0, false, 0, "different revision"));
    ASSERT_EQ(providerFrame(a.engine), SWMM_OK);
    ASSERT_NO_THROW(until(a.engine, 4.2));
    const auto cp = root() / "catchment-binding.chk",
               before = root() / "catchment-binding-before.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    const auto initial = bytes(before);
    EXPECT_FALSE(b.engine.loadCheckpoint(cp.string()).empty());
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(before), initial);
    const auto end = a.stage.completedEnd(), rain = a.sources.ledgers()[0].rain;
    SWMM_CouplingForcing bad{sizeof(bad), 1, 0, 1, .1, 10, 0, nullptr};
    SWMM_CouplingFrame frame{sizeof(frame), 1, "other-owner", 0, nullptr, 0,
                             nullptr,       1, &bad,          0, nullptr};
    EXPECT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&a.engine), &frame),
              SWMM_ERR_BADPARAM);
    EXPECT_DOUBLE_EQ(a.stage.completedEnd(), end);
    EXPECT_DOUBLE_EQ(a.sources.ledgers()[0].rain, rain);
}
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
TEST(EngineCheckpoint, CatchmentProviderNestedDamagePreservesPrivateStoresAndBothHeaders) {
    SurfaceSession a;
    ASSERT_NO_THROW(startCatchmentProvider(a, "catchment-atomic-a", 2, 0, true, 2));
    ASSERT_EQ(providerFrame(a.engine), SWMM_OK);
    ASSERT_NO_THROW(until(a.engine, 4.2));
    const auto cp = root() / "catchment-atomic.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    for (int mutation = 0; mutation < 9; ++mutation) {
        SCOPED_TRACE(mutation);
        const auto name = "catchment-atomic-" + std::to_string(mutation);
        SurfaceSession b;
        ASSERT_NO_THROW(startCatchmentProvider(b, name, 2, 0, true, 2));
        const auto before = root() / (name + "-before.chk"), bad = root() / (name + "-bad.chk");
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        const auto initial = bytes(before);
        const auto binary = bytes(root() / (name + ".out")), mesh = bytes(root() / (name + ".h5"));
        write(bad, changeProviderRecord(bytes(cp), mutation));
        EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), initial);
        EXPECT_EQ(bytes(root() / (name + ".out")), binary);
        EXPECT_EQ(bytes(root() / (name + ".h5")), mesh);
        ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
        compareCatchmentProvider(a, b, name + "-retry", true);
    }
}
TEST(EngineCheckpoint, CatchmentProviderCombinedReportsRetainPrivateAndMeshWeatherAcrossExpiry) {
    for (int lid : {0, 1, 2})
        for (int format : {1, 2}) {
            const auto name =
                "catchment-output-" + std::to_string(lid) + "-" + std::to_string(format);
            SCOPED_TRACE(name);
            SurfaceSession a, b;
            ASSERT_NO_THROW(startCatchmentProvider(a, name, lid, 0, true, format));
            ASSERT_NO_THROW(startCatchmentProvider(b, name + "-resumed", lid, 0, true, format));
            ASSERT_EQ(providerFrame(a.engine), SWMM_OK);
            ASSERT_NO_THROW(until(a.engine, 4.2));
            const auto cp = root() / (name + ".chk");
            ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
            ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
            while (a.engine.context().elapsed_ms < a.engine.context().options.totalDurationMs()) {
                ASSERT_NO_THROW(step(a.engine));
                ASSERT_NO_THROW(step(b.engine));
            }
            compareCatchmentProvider(a, b, name + "-final", true);
            ASSERT_EQ(a.engine.end(), 0);
            ASSERT_EQ(b.engine.end(), 0);
            sameBinary(name, name + "-resumed", 80);
            sameMesh(name, name + "-resumed", 40);
        }
}
TEST(EngineCheckpoint, CatchmentProviderDueDriverAndAuthoredBoundaryRestoreTogether) {
    SurfaceSession a, b;
    TestWaterDriver da, db;
    ASSERT_NO_THROW(startCatchmentProvider(a, "catchment-driver-a", 2, 0, true, 2,
                                           "mesh water driver revision 1", true));
    ASSERT_NO_THROW(startCatchmentProvider(b, "catchment-driver-b", 2, 0, true, 2,
                                           "mesh water driver revision 1", true));
    ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(da), "");
    ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(db), "");
    ASSERT_EQ(authoredBoundaryFrame(a.engine), SWMM_OK);
    ASSERT_NO_THROW(drive(a.engine, da, 4.2));
    const auto cp = root() / "catchment-driver.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
    EXPECT_EQ(da.cursor, db.cursor);
    EXPECT_DOUBLE_EQ(da.nextUpdateSeconds(), db.nextUpdateSeconds());
    ASSERT_EQ(authoredBoundaryFrame(a.engine, 2), SWMM_OK);
    ASSERT_EQ(authoredBoundaryFrame(b.engine, 2), SWMM_OK);
    ASSERT_NO_THROW(drive(a.engine, da, 13.1));
    ASSERT_NO_THROW(drive(b.engine, db, 13.1));
    compareCatchmentProvider(a, b, "catchment-driver", true);
    compareBoundary(a, b, "catchment-driver-boundary", true);
    EXPECT_EQ(da.cursor, db.cursor);
}
#endif
#endif
#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, CatchmentProviderAdvanceToPublishesInactivePendingBatchBeforeCheckpoint) {
    for (int lid : {0, 1, 2})
        for (int closure : {0, 1, 2}) {
            const auto name =
                "catchment-advance-" + std::to_string(lid) + "-" + std::to_string(closure);
            SCOPED_TRACE(name);
            SurfaceSession a, b;
            ASSERT_NO_THROW(startCatchmentProvider(a, name + "-a", lid, closure));
            ASSERT_NO_THROW(startCatchmentProvider(b, name + "-b", lid, closure));
            ASSERT_EQ(providerFrame(a.engine), SWMM_OK);
            ASSERT_NO_THROW(until(a.engine, 13.1));
            ASSERT_FALSE(a.engine.surfaceRouter2D().runtimeActive());
            double actual = 0;
            ASSERT_EQ(a.engine.advanceTo(16.125, &actual), SWMM_OK);
            EXPECT_DOUBLE_EQ(actual, 16.125);
            EXPECT_EQ(a.engine.sourceDelivery()->capturedCount(), a.sources.deliveries().size());
            const auto cp = root() / (name + ".chk");
            ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
            ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
            compareCatchmentProvider(a, b, name + "-restored");
            double resumed = 0;
            ASSERT_EQ(a.engine.advanceTo(19.125, &actual), SWMM_OK);
            ASSERT_EQ(b.engine.advanceTo(19.125, &resumed), SWMM_OK);
            EXPECT_DOUBLE_EQ(actual, resumed);
            compareCatchmentProvider(a, b, name + "-continued");
        }
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, RuntimeExpiryPreservesAdjacentFutureTimesUntilTheirExactBoundary) {
    for (double now : {0., 1.25, 1000000.}) {
        SCOPED_TRACE(now);
        const double end = std::nextafter(now, std::numeric_limits<double>::infinity());
        twoD::RuntimeSources sources;
        twoD::RuntimeSource source;
        source.id = "future";
        source.cell = 0;
        source.until = end;
        sources.replace(source);
        sources.rebuild(1);
        twoD::RuntimeForcings forcings;
        twoD::RuntimeForcing rain;
        rain.channel = 1;
        rain.cell = 0;
        rain.until = end;
        forcings.replace(rain);
        forcings.rebuild(1);
        EXPECT_FALSE(sources.expire(now));
        forcings.expire(now);
        EXPECT_TRUE(sources.active());
        EXPECT_NE(forcings.get(1, 0), nullptr);
        EXPECT_DOUBLE_EQ(sources.nextTime(now), end);
        EXPECT_DOUBLE_EQ(forcings.nextTime(now), end);
        EXPECT_TRUE(sources.expire(end));
        forcings.expire(end);
        EXPECT_FALSE(sources.active());
        EXPECT_EQ(forcings.get(1, 0), nullptr);
        EXPECT_TRUE(sources.cells[0].empty());
    }
}
TEST(EngineCheckpoint, CatchmentSchedulerDueAdvancePreservesPendingWorkAndCheckpoint) {
    SurfaceSession s;
    TestWaterDriver driver;
    ASSERT_NO_THROW(startCatchmentProvider(s, "schedule-due", 2));
    ASSERT_EQ(s.engine.attachRuntimeWaterDriverCheckpoint(driver), "");
    ASSERT_NO_THROW(driver.send(s.engine));
    const SWMM_CouplingClear clears[] = {
        {0, -1, "in"}, {0, -1, "out"}, {1, -1, "in"}, {1, -1, "out"}};
    const SWMM_CouplingForcing off[] = {{sizeof(SWMM_CouplingForcing), 1, -1, 0, 0, 0, 0, nullptr},
                                        {sizeof(SWMM_CouplingForcing), 1, 0, 0, 0, 0, 0, nullptr},
                                        {sizeof(SWMM_CouplingForcing), 2, -1, 0, 0, 0, 0, nullptr},
                                        {sizeof(SWMM_CouplingForcing), 3, 0, 0, 0, 0, 0, nullptr}};
    const SWMM_CouplingFrame clear{sizeof(clear), 1, "water", 0, nullptr, 0,
                                   nullptr,       4, off,     4, clears};
    ASSERT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&s.engine), &clear), SWMM_OK);
    ASSERT_FALSE(s.engine.surfaceRouter2D().runtimeActive());
    ASSERT_NO_THROW(until(s.engine, 4.2));
    ASSERT_DOUBLE_EQ(s.engine.context().current_time, driver.nextUpdateSeconds());
    ASSERT_LT(s.stage.completedEnd(), s.engine.context().current_time);
    const auto before = root() / "schedule-due-before.chk",
               after = root() / "schedule-due-after.chk";
    ASSERT_EQ(s.engine.saveCheckpoint(before.string()), "");
    const auto saved = bytes(before), state = driverState(driver, 4.2);
    double actual = -1;
    EXPECT_EQ(s.engine.advanceTo(5, &actual), SWMM_ERR_LIFECYCLE);
    EXPECT_DOUBLE_EQ(actual, 4.2);
    ASSERT_EQ(s.engine.saveCheckpoint(after.string()), "");
    EXPECT_EQ(bytes(after), saved);
    EXPECT_EQ(driverState(driver, 4.2), state);
    ASSERT_NO_THROW(driver.send(s.engine));
    ASSERT_EQ(s.engine.advanceTo(5, &actual), SWMM_OK);
    EXPECT_DOUBLE_EQ(actual, 5);
}
TEST(EngineCheckpoint, CatchmentSchedulerAdvanceReportsFlushFailureWithoutTerminating) {
    SurfaceSession s;
    ASSERT_NO_THROW(startCatchmentProvider(s, "schedule-flush-failure"));
    ASSERT_NO_THROW(until(s.engine, 1.4));
    ASSERT_LT(s.stage.completedEnd(), s.engine.context().current_time);
    auto& clock = const_cast<runoff::SourceClockGroups&>(s.sources.clocks());
    ASSERT_EQ(clock.stage(0, 2, 2), ""); // Deliberately unsettled trusted internal source trial.
    const auto now = s.engine.context().current_time;
    double actual = -1;
    EXPECT_NE(s.engine.advanceTo(now, &actual), SWMM_OK);
    EXPECT_DOUBLE_EQ(actual, now);
    EXPECT_FALSE(s.engine.context().error_message.empty());
}
TEST(EngineCheckpoint, CatchmentSchedulerRefinementPreservesWeightedRainAndExactExpiry) {
    constexpr double ft2 = .3048 * .3048, ft3 = ft2 * .3048;
    for (int lid : {0, 1, 2})
        for (int closure : {0, 1, 2})
            for (double cadence : {.7, .35, .175}) {
                const auto name = "schedule-refine-" + std::to_string(lid) + "-" +
                                  std::to_string(closure) + "-" + std::to_string(cadence);
                SCOPED_TRACE(name);
                SurfaceSession s;
                s.start(name, lid, 1, closure, 0, true, true, false, -1, false, 0, false,
                        "mesh water driver revision 1", false, 0, 0, true, cadence);
                auto native = s.sources.clocks();
                ASSERT_EQ(native.stage(0, 4.375, 4.375), "");
                std::vector<double> expected(s.sources.context().n_subcatches(), 0);
                const auto& area = s.sources.runoff().soa().area;
                for (const auto& f : native.groups()[0].pending)
                    for (const auto& w : f.sources) {
                        const double full =
                            (area[w.subcatch] +
                             s.sources.context().subcatches.total_lid_area_ft2[w.subcatch]) *
                            ft2;
                        expected[w.subcatch] += (full - .5) * w.rain * .3048 * (f.end - f.start);
                        expected[w.subcatch] +=
                            .5 *
                            (w.subcatch % 2 == 0 ? .002 * 2.125 + .001 * (4.375 - 2.125)
                                                 : .001 * 4.375) *
                            (f.end - f.start) / 4.375;
                    }
                const SWMM_CouplingForcing rows[] = {
                    {sizeof(SWMM_CouplingForcing), 1, -1, 1, .001, 4.375, 0, nullptr},
                    {sizeof(SWMM_CouplingForcing), 1, 0, 1, .002, 2.125, 0, nullptr}};
                const SWMM_CouplingFrame frame{sizeof(frame), 1, "water", 0, nullptr, 0,
                                               nullptr,       2, rows,    0, nullptr};
                ASSERT_EQ(
                    swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&s.engine), &frame),
                    SWMM_OK);
                double actual = -1;
                ASSERT_EQ(s.engine.advanceTo(2.125, &actual), SWMM_OK);
                EXPECT_DOUBLE_EQ(actual, 2.125);
                EXPECT_FALSE(
                    s.engine.surfaceRouter2D().state().runtime_forcings.find(1, 0)->enabled);
                EXPECT_TRUE(
                    s.engine.surfaceRouter2D().state().runtime_forcings.find(1, -1)->enabled);
                ASSERT_EQ(s.engine.advanceTo(4.375, &actual), SWMM_OK);
                EXPECT_DOUBLE_EQ(actual, 4.375);
                EXPECT_FALSE(s.engine.surfaceRouter2D().runtimeActive());
                EXPECT_DOUBLE_EQ(s.stage.completedEnd(), 4.375);
                for (const auto& group : s.sources.clocks().groups())
                    for (int sc : group.subcatches)
                        EXPECT_NEAR(s.sources.ledgers()[sc].rain * ft3, expected[sc],
                                    1e-10 * std::max(1., expected[sc]));
                EXPECT_NEAR(s.stage.meshRain(),
                            3.5 * (.002 * 2.125 + .001 * (4.375 - 2.125)) + 3.5 * .001 * 4.375,
                            1e-12);
                EXPECT_NEAR(s.sources.balanceResidual(0), 0, 1e-6);
                const auto cp = root() / (name + ".chk");
                EXPECT_EQ(s.engine.saveCheckpoint(cp.string()), "");
            }
}

#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, CatchmentSchedulerRefusesCheckpointCallbacksBeforePhysicalAdvance) {
    SurfaceSession s;
    ASSERT_NO_THROW(startCatchmentProvider(s, "schedule-callback-refusal", 2));
    const auto volume = s.engine.surfaceRouter2D().state().volume;
    const auto source = s.sources.runoff().soa().depth_perv;
    int calls = 0;
    s.engine.set_step_begin_callback(
        [](void*, double, double, void* data) { ++*static_cast<int*>(data); }, &calls);
    double elapsed = -1;
    EXPECT_NE(s.engine.step(&elapsed), SWMM_OK);
    EXPECT_EQ(calls, 0);
    EXPECT_DOUBLE_EQ(s.engine.context().current_time, 0);
    EXPECT_EQ(s.stage.intervals(), 0);
    EXPECT_EQ(s.engine.surfaceRouter2D().state().volume, volume);
    EXPECT_EQ(s.sources.runoff().soa().depth_perv, source);
    EXPECT_NE(std::string(s.engine.last_error_message()).find("callbacks"), std::string::npos);
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, CatchmentSchedulerHistoryRefusesUnqualifiedBarriersWithoutMutation) {
    for (bool detailed : {false, true})
        for (int lid : {0, 1, 2}) {
            const auto name =
                "schedule-history-guard-" + std::to_string(detailed) + "-" + std::to_string(lid);
            SCOPED_TRACE(name);
            twoD::EngineSourceResultHistory history;
            SurfaceSession s;
            ASSERT_NO_THROW(startCatchmentProvider(s, name, lid));
            ASSERT_EQ(s.engine.setEngineSourceResultHistory(history, detailed), "");
            ASSERT_NO_THROW(until(s.engine, 1.4));
            ASSERT_LT(s.stage.completedEnd(), s.engine.context().current_time);
            const auto before = root() / (name + "-before.chk"),
                       after = root() / (name + "-after.chk");
            ASSERT_EQ(s.engine.saveCheckpoint(before.string()), "");
            const auto saved = bytes(before);
            const auto time = s.engine.context().current_time;
            const auto tables = history.tables();
            const auto source_tables = detailed ? history.sourceIntervals()->tables()
                                                : std::map<std::string, twoD::SourceResultTable>{};
            SWMM_CouplingForcing row{sizeof(row), 1, 0, 1, .003, 5, 0, nullptr};
            SWMM_CouplingFrame f{sizeof(f), 1, "water", 0, nullptr, 0,
                                 nullptr,   1, &row,    0, nullptr};
            EXPECT_NE(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&s.engine), &f),
                      SWMM_OK);
            double actual = -1;
            EXPECT_EQ(s.engine.advanceTo(time, &actual), SWMM_ERR_LIFECYCLE);
            EXPECT_DOUBLE_EQ(actual, time);
            ASSERT_EQ(s.engine.saveCheckpoint(after.string()), "");
            EXPECT_EQ(bytes(after), saved);
            for (const auto& [key, t] : tables)
                EXPECT_EQ(history.tables().at(key).columns, t.columns);
            for (const auto& [key, t] : source_tables)
                EXPECT_EQ(history.sourceIntervals()->tables().at(key).columns, t.columns);
            EXPECT_DOUBLE_EQ(s.engine.context().current_time, time);
            EXPECT_EQ(s.engine.context().error_code, 0);
            // A real accepted step settles pending work through the existing capture
            // path and remains supported; the refusal does not poison the session.
            ASSERT_EQ(s.engine.advanceTo(2.125, &actual), SWMM_OK);
            EXPECT_DOUBLE_EQ(actual, 2.125);
            EXPECT_EQ(s.engine.saveCheckpoint(after.string()), "");
        }
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, CatchmentSchedulerFramesValidateExpiryAgainstAcceptedSourceClock) {
    SurfaceSession s;
    s.start("schedule-frame-clock", 2, 1, 0, 0, true, true, false, -1, false, 0, false,
            "mesh water driver revision 1", false, 0, 0, true, .1);
    for (int i = 0; i < 3; ++i)
        ASSERT_NO_THROW(step(s.engine));
    const double now = s.engine.context().current_time;
    ASSERT_GT(now,
              s.engine.context().elapsed_ms / 1000.0); // Separate FP recurrences differ by one ULP.
    const auto before = root() / "schedule-frame-clock-before.chk",
               after = root() / "schedule-frame-clock-after.chk";
    ASSERT_EQ(s.engine.saveCheckpoint(before.string()), "");
    const auto saved = bytes(before);
    SWMM_CouplingForcing rain{sizeof(rain), 1, 0, 1, .001, now, 0, nullptr};
    SWMM_CouplingFrame f{sizeof(f), 1, "water", 0, nullptr, 0, nullptr, 1, &rain, 0, nullptr};
    EXPECT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&s.engine), &f),
              SWMM_ERR_BADPARAM);
    ASSERT_EQ(s.engine.saveCheckpoint(after.string()), "");
    EXPECT_EQ(bytes(after), saved);
    rain.until_seconds = std::nextafter(now, std::numeric_limits<double>::infinity());
    ASSERT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&s.engine), &f), SWMM_OK);
    const auto* row = s.engine.surfaceRouter2D().state().runtime_forcings.find(1, 0);
    ASSERT_NE(row, nullptr);
    EXPECT_TRUE(row->enabled);
    EXPECT_GT(row->until, now);
    EXPECT_EQ(s.engine.saveCheckpoint(after.string()), "");
}
#endif

#ifdef OPENSWMM_HAS_2D
namespace {
using Stream = twoD::EngineSourceResultStream;
/**
 * @brief Remove an old source-stream artifact and return its fresh destination path.
 *
 * Deletes an existing matching file via throwing filesystem remove, then returns without creating
 * it. Caller owns later stream creation; root directory initialization/cleanup behavior applies.
 *
 * @param[in] name Raw artifact stem below root; .source-stream appended.
 * @return Owned destination path, which is absent after successful removal.
 */
std::filesystem::path streamPath(const std::string& name) {
    auto p = root() / (name + ".source-stream");
    std::filesystem::remove(p);
    return p;
}
/**
 * @brief Compare published source-stream cursors and exact file contents.
 *
 * Nonfatally compares cursor values and unchecked bytes(path) equality. Does not flush streams or
 * independently assert file readability; callers ensure evidence files are available/quiescent for
 * meaningful comparison.
 *
 * @param[in] a Borrowed reference stream with stable/published file contents.
 * @param[in] b Borrowed candidate stream at the expected same cursor.
 */
void equalStreamPrefix(const Stream& a, const Stream& b) {
    EXPECT_EQ(a.cursor(), b.cursor());
    EXPECT_TRUE(bytes(a.path()) == bytes(b.path())) << "Stream bytes differ.";
}
/**
 * @brief Compare one streamed frame table to its matching in-memory history rows.
 *
 * Fatally requires schema/units and selected row count, locates equal-frame range using
 * lower/upper_bound, then exactly compares every value. Column zero compares expected frame rather
 * than actual stored column-zero values; other columns use optional row offset. Requires nonempty
 * first history column and compatible column counts; does not mutate tables or validate arbitrary
 * malformed table shapes.
 *
 * @param[in] actual Borrowed valid frame table with internally consistent fields/units/columns.
 * @param[in] history Borrowed complete history table whose first column contains sorted frame
 *     indices.
 * @param[in] frame Expected frame index converted to double for the history selection.
 * @param[in] row_index Add history-row offset to actual column one when true, reconciling
 *     local/global row numbering.
 */
void matchStreamTable(const twoD::SourceResultTable& actual, const twoD::SourceResultTable& history,
                      std::size_t frame, bool row_index = false) {
    ASSERT_EQ(actual.fields, history.fields);
    ASSERT_EQ(actual.units, history.units);
    auto begin =
        std::lower_bound(history.columns[0].begin(), history.columns[0].end(), double(frame));
    auto end = std::upper_bound(begin, history.columns[0].end(), double(frame));
    const auto offset = std::size_t(begin - history.columns[0].begin());
    ASSERT_EQ(actual.rows(), std::size_t(end - begin));
    for (std::size_t c = 0; c < actual.columns.size(); ++c)
        for (std::size_t r = 0; r < actual.rows(); ++r)
            EXPECT_DOUBLE_EQ(c == 0 ? double(frame)
                                    : actual.columns[c][r] +
                                          (row_index && c == 1 ? double(offset) : 0),
                             history.columns[c][offset + r])
                << actual.fields[c];
}
}
TEST(EngineCheckpoint, StreamPreservesEveryQualifiedStepAndDetailedIntervalWithBoundedRetention) {
    for (bool detailed : {false, true})
        for (int lid : {0, 1, 2}) {
            const auto name =
                "stream-coverage-" + std::to_string(detailed) + "-" + std::to_string(lid);
            SCOPED_TRACE(name);
            Stream recorded;
            twoD::EngineSourceResultHistory history;
            SurfaceSession a, control;
            ASSERT_NO_THROW(startClosedSpatial(a, name, lid, .7, true, lid == 2));
            ASSERT_NO_THROW(
                startClosedSpatial(control, name + "-control", lid, .7, true, lid == 2));
            ASSERT_EQ(
                a.engine.setEngineSourceResultStream(recorded, streamPath(name).string(), detailed),
                "");
            ASSERT_EQ(control.engine.setEngineSourceResultHistory(history, detailed), "");
            for (int i = 0; i < 32; ++i) {
                ASSERT_NO_THROW(step(a.engine));
                ASSERT_NO_THROW(step(control.engine));
                compare(a.engine, control.engine, name);
            }
            std::size_t steps = 0, intervals = 0;
            ASSERT_EQ(
                Stream::visit(
                    recorded.path(), recorded.cursor(),
                    [&](const Stream::Frame& frame) {
                        if (frame.kind == Stream::Kind::SourceInterval) {
                            ASSERT_TRUE(detailed);
                            ++intervals;
                            for (const auto& [key, t] : frame.tables) {
                                const auto name = key.substr(7);
                                const bool indexed = name.starts_with("intake") ||
                                                     name.starts_with("return") ||
                                                     name.starts_with("pending_");
                                // Mass row ids refer to the corresponding water table, whose
                                // offset can differ from the number of mass rows (water-only fixture).
                                matchStreamTable(t, history.sourceIntervals()->tables().at(name),
                                                 intervals, indexed);
                            }
                        } else {
                            ASSERT_TRUE(frame.kind == Stream::Kind::Initial ||
                                        frame.kind == Stream::Kind::EngineStep);
                            for (const auto& [key, t] : frame.tables)
                                if (key.starts_with("engine/")) {
                                    const auto name = key.substr(7);
                                    matchStreamTable(t, history.tables().at(name), steps,
                                                     name.starts_with("pending_"));
                                }
                            ++steps;
                        }
                    }),
                "");
            EXPECT_EQ(steps, 33);
            EXPECT_EQ(intervals, detailed ? std::size_t(a.stage.intervals()) : 0);
            EXPECT_GT(recorded.cursor().bytes, recorded.largestFrameBytes() * 10);
            EXPECT_EQ(recorded.latest().tables.at("engine/boundaries").rows(), 1);
            const auto retained = recorded.latest().tables.at("engine/node_delivery").rows();
            EXPECT_EQ(retained, std::size_t(a.engine.context().n_nodes()));
        }
}
TEST(EngineCheckpoint,
     StreamRecordsCompletedBarriersAndPrescriptionsWithoutReplayingRoutingReceipts) {
    for (bool detailed : {false, true})
        for (int lid : {0, 1, 2}) {
            const auto name =
                "stream-barriers-" + std::to_string(detailed) + "-" + std::to_string(lid);
            SCOPED_TRACE(name);
            Stream recorded;
            SurfaceSession a;
            ASSERT_NO_THROW(startCatchmentProvider(a, name, lid));
            ASSERT_EQ(
                a.engine.setEngineSourceResultStream(recorded, streamPath(name).string(), detailed),
                "");
            ASSERT_NO_THROW(until(a.engine, 1.4));
            ASSERT_LT(a.stage.completedEnd(), a.engine.context().current_time);
            const auto at = a.engine.context().current_time;
            const auto prefix = recorded.cursor();
            const auto previous = recorded.latest().tables.at("engine/node_delivery");
            double actual = -1;
            ASSERT_EQ(a.engine.advanceTo(at, &actual), SWMM_OK);
            EXPECT_DOUBLE_EQ(actual, at);
            EXPECT_EQ(recorded.latest().kind, Stream::Kind::CouplingBarrier);
            EXPECT_DOUBLE_EQ(recorded.latest().engine_start, at);
            EXPECT_DOUBLE_EQ(recorded.latest().engine_end, at);
            EXPECT_DOUBLE_EQ(a.stage.completedEnd(), at);
            EXPECT_EQ(recorded.latest().tables.at("engine/node_delivery").columns[4],
                      previous.columns[4]);
            ASSERT_EQ(Stream::visit(recorded.path(), prefix, [](const auto&) {}),
                      ""); // Earlier snapshots remain immutable.
            SWMM_CouplingForcing rain{sizeof(rain), 1, 0, 1, .003, 2.125, 0, nullptr};
            SWMM_CouplingFrame f{sizeof(f), 1, "water", 0, nullptr, 0,
                                 nullptr,   1, &rain,   0, nullptr};
            ASSERT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&a.engine), &f),
                      SWMM_OK);
            EXPECT_EQ(recorded.latest().kind, Stream::Kind::PrescriptionUpdate);
            EXPECT_DOUBLE_EQ(recorded.latest().engine_end, at);
            const auto installed = recorded.cursor();
            rain.until_seconds = at;
            EXPECT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&a.engine), &f),
                      SWMM_ERR_BADPARAM);
            EXPECT_EQ(recorded.cursor(), installed);
            ASSERT_EQ(a.engine.advanceTo(2.125, &actual), SWMM_OK);
            EXPECT_DOUBLE_EQ(actual, 2.125);
            int barriers = 0, updates = 0;
            ASSERT_EQ(Stream::visit(recorded.path(), recorded.cursor(),
                                    [&](const auto& frame) {
                                        if (frame.kind == Stream::Kind::CouplingBarrier ||
                                            frame.kind == Stream::Kind::PrescriptionUpdate) {
                                            ++barriers;
                                            updates +=
                                                frame.kind == Stream::Kind::PrescriptionUpdate;
                                            const auto& n =
                                                frame.tables.at("engine/node_delivery").columns;
                                            for (std::size_t r = 0; r < n[0].size(); ++r) {
                                                EXPECT_EQ(n[6][r], 0);
                                                EXPECT_EQ(n[7][r], 0);
                                                EXPECT_EQ(n[10][r], 0);
                                            }
                                        }
                                    }),
                      "");
            EXPECT_GE(barriers, 3);
            EXPECT_EQ(updates, 1);
        }
}
TEST(EngineCheckpoint, StreamMissingChangedAndTruncatedPrefixesRefuseBeforeRestoreAndPermitRetry) {
    Stream recorded;
    SurfaceSession a;
    ASSERT_NO_THROW(startCatchmentProvider(a, "stream-prefix-source", 2));
    const auto source = streamPath("stream-prefix-source");
    ASSERT_EQ(a.engine.setEngineSourceResultStream(recorded, source.string(), true), "");
    ASSERT_EQ(providerFrame(a.engine, 2.125), SWMM_OK);
    double actual;
    ASSERT_EQ(a.engine.advanceTo(2.125, &actual), SWMM_OK);
    const auto checkpoint = root() / "stream-prefix.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()), "");
    const auto cursor = recorded.cursor();
    const auto good = bytes(source);
    // Later valid frames are outside the saved prefix and must not be copied on branching.
    ASSERT_EQ(a.engine.advanceTo(3.5, &actual), SWMM_OK);
    ASSERT_GT(recorded.cursor().bytes, cursor.bytes);
    const auto extended = bytes(source);
    for (int variant = 0; variant < 5; ++variant) {
        SCOPED_TRACE(variant);
        Stream target;
        SurfaceSession b;
        const auto name = "stream-prefix-target-" + std::to_string(variant);
        ASSERT_NO_THROW(startCatchmentProvider(b, name, 2));
        const auto destination = streamPath(name);
        ASSERT_EQ(b.engine.setEngineSourceResultStream(target, destination.string(), true), "");
        const auto fresh = bytes(destination);
        const auto initial = target.cursor();
        auto changed = good;
        if (variant == 0)
            changed.resize(changed.size() - 1);
        if (variant == 1)
            changed[16] ^= 1;
        if (variant == 2)
            changed[changed.size() / 2] ^= 1;
        if (variant == 3)
            changed.back() ^= 1;
        if (variant == 4)
            std::filesystem::rename(source, source.string() + ".missing");
        else
            write(source, changed);
        EXPECT_FALSE(b.engine.loadCheckpoint(checkpoint.string()).empty());
        EXPECT_DOUBLE_EQ(b.engine.context().current_time, 0);
        EXPECT_EQ(target.cursor(), initial);
        EXPECT_EQ(bytes(destination), fresh);
        if (variant == 4)
            std::filesystem::rename(source.string() + ".missing", source);
        else
            write(source, extended);
        ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()), "");
        EXPECT_EQ(target.cursor(), cursor);
        EXPECT_EQ(bytes(destination), good);
        ASSERT_EQ(Stream::visit(destination.string(), target.cursor(), [](const auto&) {}), "");
        double restored;
        ASSERT_EQ(b.engine.advanceTo(3.5, &restored), SWMM_OK);
        EXPECT_DOUBLE_EQ(restored, actual);
        compareCatchmentProvider(a, b, name);
        equalStreamPrefix(recorded, target);
    }
}
TEST(EngineCheckpoint, StreamWriteRefusalPreservesPhysicalBoundaryAndDurablePrefix) {
    Stream recorded;
    SurfaceSession a;
    ASSERT_NO_THROW(startCatchmentProvider(a, "stream-write-refusal", 2));
    const auto path = streamPath("stream-write-refusal");
    ASSERT_EQ(a.engine.setEngineSourceResultStream(recorded, path.string(), true), "");
    ASSERT_NO_THROW(until(a.engine, 1.4));
    const auto before = recorded.cursor();
    const auto saved = bytes(path);
    const auto time = a.engine.context().current_time, end = a.stage.completedEnd();
    std::filesystem::permissions(path, std::filesystem::perms::owner_read);
    EXPECT_NE(a.engine.step(nullptr), SWMM_OK);
    EXPECT_DOUBLE_EQ(a.engine.context().current_time, time);
    EXPECT_DOUBLE_EQ(a.stage.completedEnd(), end);
    EXPECT_EQ(recorded.cursor(), before);
    EXPECT_EQ(bytes(path), saved);
    SWMM_CouplingForcing rain{sizeof(rain), 1, 0, 1, .003, 5, 0, nullptr};
    SWMM_CouplingFrame f{sizeof(f), 1, "water", 0, nullptr, 0, nullptr, 1, &rain, 0, nullptr};
    EXPECT_NE(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&a.engine), &f), SWMM_OK);
    EXPECT_DOUBLE_EQ(a.stage.completedEnd(), end);
    EXPECT_EQ(recorded.cursor(), before);
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write);
    ASSERT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&a.engine), &f), SWMM_OK);
    ASSERT_NO_THROW(step(a.engine));
    ASSERT_EQ(Stream::visit(path.string(), recorded.cursor(), [](const auto&) {}), "");
    const auto intact = bytes(path);
    const auto accepted = recorded.cursor();
    write(path, intact + "partial");
    EXPECT_NE(a.engine.step(nullptr), SWMM_OK);
    EXPECT_EQ(recorded.cursor(), accepted);
    EXPECT_FALSE(a.engine.saveCheckpoint((root() / "stream-partial.chk").string()).empty());
    write(path, intact);
    ASSERT_NO_THROW(step(a.engine));
}
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
TEST(EngineCheckpoint, StreamRestoresWithDriverBinaryAndMeshOutputsAsOnePublication) {
    for (bool detailed : {false, true})
        for (int lid : {0, 1, 2}) {
            const auto name =
                "stream-combined-" + std::to_string(detailed) + "-" + std::to_string(lid);
            SCOPED_TRACE(name);
            Stream recorded, resumed;
            TestWaterDriver da, db;
            SurfaceSession a, b;
            ASSERT_NO_THROW(startCatchmentProvider(a, name, lid, 0, true, 2));
            ASSERT_NO_THROW(startCatchmentProvider(b, name + "-resumed", lid, 0, true, 2));
            ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(da), "");
            ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(db), "");
            ASSERT_EQ(
                a.engine.setEngineSourceResultStream(recorded, streamPath(name).string(), detailed),
                "");
            ASSERT_EQ(b.engine.setEngineSourceResultStream(
                          resumed, streamPath(name + "-resumed").string(), detailed),
                      "");
            ASSERT_EQ(providerFrame(a.engine), SWMM_OK);
            ASSERT_NO_THROW(drive(a.engine, da, 4.2));
            const auto due = recorded.cursor();
            const auto due_bytes = bytes(recorded.path());
            double actual = -1;
            EXPECT_EQ(a.engine.advanceTo(5, &actual), SWMM_ERR_LIFECYCLE);
            EXPECT_DOUBLE_EQ(actual, 4.2);
            EXPECT_EQ(recorded.cursor(), due);
            EXPECT_TRUE(bytes(recorded.path()) == due_bytes);
            const auto cp = root() / (name + ".chk");
            ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
            const auto binary = root() / (name + "-resumed.out"),
                       mesh = root() / (name + "-resumed.h5");
            const auto bh = bytes(binary), mh = bytes(mesh), sh = bytes(resumed.path());
            const auto initial = resumed.cursor();
            const auto driver = driverState(db, 0);
            std::filesystem::permissions(mesh, std::filesystem::perms::owner_read);
            EXPECT_FALSE(b.engine.loadCheckpoint(cp.string()).empty());
            EXPECT_DOUBLE_EQ(b.engine.context().current_time, 0);
            EXPECT_EQ(bytes(binary), bh);
            EXPECT_EQ(bytes(mesh), mh);
            EXPECT_EQ(bytes(resumed.path()), sh);
            EXPECT_EQ(resumed.cursor(), initial);
            EXPECT_EQ(driverState(db, 0), driver);
            std::filesystem::permissions(mesh, std::filesystem::perms::owner_read |
                                                   std::filesystem::perms::owner_write);
            ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
            compareCatchmentProvider(a, b, name + "-restored", true);
            equalStreamPrefix(recorded, resumed);
            ASSERT_NO_THROW(drive(a.engine, da, 25));
            ASSERT_NO_THROW(drive(b.engine, db, 25));
            compareCatchmentProvider(a, b, name + "-continued", true);
            equalStreamPrefix(recorded, resumed);
            ASSERT_EQ(a.engine.end(), 0);
            ASSERT_EQ(b.engine.end(), 0);
            sameBinary(name, name + "-resumed", 8);
            sameMesh(name, name + "-resumed", 4);
        }
}
#endif
#endif

#if defined(OPENSWMM_HAS_2D) && !defined(_WIN32)
TEST(EngineCheckpoint,
     StreamPartialFrameFailurePreservesPrefixAndRetriesAcceptedCaptureWithoutRoutingAgain) {
    Stream recorded;
    SurfaceSession a;
    ASSERT_NO_THROW(startCatchmentProvider(a, "stream-partial-write", 2));
    const auto path = streamPath("stream-partial-write");
    ASSERT_EQ(a.engine.setEngineSourceResultStream(recorded, path.string(), false), "");
    ASSERT_NO_THROW(until(a.engine, 1.4));
    const auto cursor = recorded.cursor();
    const auto prefix = bytes(path);
    const double before = a.engine.context().current_time;
    /** @cond */
    struct LimitedFile {
        rlimit previous{};
        decltype(std::signal(SIGXFSZ, SIG_IGN)) handler;
        /**
         * @brief Temporarily constrain process output file size for partial-write testing.
         *
         * Saves current file-size limit and installs SIGXFSZ ignore before querying/applying the limit.
         * Throws runtime_error on getrlimit/setrlimit failure; because construction then fails, the changed
         * signal handler is not automatically restored. Successful construction requires serialized
         * process-global resource/signal manipulation and teardown via this local owner.
         *
         * @param[in] bytes New RLIMIT_FSIZE soft limit in bytes, no greater than the inherited hard limit.
         */
        explicit LimitedFile(uint64_t bytes) : handler(std::signal(SIGXFSZ, SIG_IGN)) {
            if (getrlimit(RLIMIT_FSIZE, &previous))
                throw std::runtime_error("Cannot read test file-size limit.");
            auto changed = previous;
            changed.rlim_cur = bytes;
            if (setrlimit(RLIMIT_FSIZE, &changed))
                throw std::runtime_error("Cannot set test file-size limit.");
        }
        /**
         * @brief Restore the saved process file-size limit and SIGXFSZ handler.
         *
         * Ignores restoration statuses. Requires successful construction; affects the whole process rather
         * than only this fixture stream. No file is closed or deleted.
         */
        ~LimitedFile() {
            setrlimit(RLIMIT_FSIZE, &previous);
            std::signal(SIGXFSZ, handler);
        }
    };
    /** @endcond */
    int result;
    {
        LimitedFile limit(cursor.bytes + 16);
        result = a.engine.step(nullptr);
    }
    EXPECT_NE(result, SWMM_OK);
    EXPECT_GT(a.engine.context().current_time, before);
    EXPECT_EQ(a.engine.context().error_code, 0);
    EXPECT_EQ(recorded.cursor(), cursor);
    EXPECT_TRUE(bytes(path) == prefix);
    EXPECT_FALSE(recorded.lastError().empty());
    const auto accepted = a.engine.context().current_time;
    const auto volumes = a.engine.surfaceRouter2D().state().volume;
    EXPECT_EQ(a.engine.step(nullptr), SWMM_ERR_LIFECYCLE);
    EXPECT_DOUBLE_EQ(a.engine.context().current_time, accepted);
    ASSERT_EQ(a.engine.saveCheckpoint((root() / "stream-partial-write.chk").string()), "");
    EXPECT_EQ(recorded.latest().kind, Stream::Kind::EngineStep);
    EXPECT_DOUBLE_EQ(recorded.latest().engine_start, before);
    EXPECT_DOUBLE_EQ(recorded.latest().engine_end, accepted);
    EXPECT_EQ(a.engine.surfaceRouter2D().state().volume, volumes);
    EXPECT_TRUE(recorded.lastError().empty());
    EXPECT_EQ(recorded.cursor().frames, cursor.frames + 1);
    ASSERT_EQ(Stream::visit(path.string(), recorded.cursor(), [](const auto&) {}), "");
    ASSERT_NO_THROW(step(a.engine));
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, StreamRestartPreservesPendingBatchesAcrossNodeAndGroundwaterProfiles) {
    for (bool detailed : {false, true})
        for (int lid : {0, 1, 2})
            for (int closure : {0, 1, 2}) {
                const auto name = "stream-pending-" + std::to_string(detailed) + "-" +
                                  std::to_string(lid) + "-" + std::to_string(closure);
                SCOPED_TRACE(name);
                Stream recorded, resumed;
                SurfaceSession a, b;
                ASSERT_NO_THROW(startCatchmentProvider(a, name, lid, closure));
                ASSERT_NO_THROW(startCatchmentProvider(b, name + "-resumed", lid, closure));
                ASSERT_EQ(a.engine.setEngineSourceResultStream(recorded, streamPath(name).string(),
                                                               detailed),
                          "");
                ASSERT_EQ(b.engine.setEngineSourceResultStream(
                              resumed, streamPath(name + "-resumed").string(), detailed),
                          "");
                ASSERT_NO_THROW(until(a.engine, 1.4));
                ASSERT_LT(a.stage.completedEnd(), a.engine.context().current_time);
                const auto cp = root() / (name + ".chk");
                ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
                ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
                compareCatchmentProvider(a, b, name + "-restored");
                equalStreamPrefix(recorded, resumed);
                for (int i = 0; i < 20; ++i) {
                    ASSERT_NO_THROW(step(a.engine));
                    ASSERT_NO_THROW(step(b.engine));
                }
                compareCatchmentProvider(a, b, name + "-continued");
                equalStreamPrefix(recorded, resumed);
            }
}
TEST(EngineCheckpoint, StreamWrongRecordingProfileAndCursorLeaveFreshTargetUnchanged) {
    Stream recorded, target;
    SurfaceSession a, b;
    ASSERT_NO_THROW(startCatchmentProvider(a, "stream-profile-source", 2));
    ASSERT_NO_THROW(startCatchmentProvider(b, "stream-profile-target", 2));
    ASSERT_EQ(a.engine.setEngineSourceResultStream(
                  recorded, streamPath("stream-profile-source").string(), true),
              "");
    ASSERT_EQ(b.engine.setEngineSourceResultStream(
                  target, streamPath("stream-profile-target").string(), false),
              "");
    twoD::SourceResultHistory component;
    EXPECT_THROW(b.engine.surfaceRouter2D().setSourceResultHistory(component),
                 std::invalid_argument);
    ASSERT_NO_THROW(until(a.engine, 1.4));
    const auto cp = root() / "stream-profile.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    const auto fresh = bytes(target.path());
    const auto cursor = target.cursor();
    EXPECT_FALSE(b.engine.loadCheckpoint(cp.string()).empty());
    EXPECT_DOUBLE_EQ(b.engine.context().current_time, 0);
    EXPECT_EQ(target.cursor(), cursor);
    EXPECT_TRUE(bytes(target.path()) == fresh);
    for (int variant = 0; variant < 4; ++variant) {
        auto wrong = recorded.cursor();
        if (variant == 0)
            ++wrong.frames;
        if (variant == 1)
            --wrong.bytes;
        if (variant == 2)
            wrong.fingerprint ^= 1;
        if (variant == 3)
            wrong.identity[0] ^= 1;
        EXPECT_FALSE(Stream::visit(recorded.path(), wrong, [](const auto&) {}).empty());
    }
}
#endif
#if defined(OPENSWMM_HAS_2D) && !defined(_WIN32)
TEST(EngineCheckpoint,
     StreamPrescriptionCaptureFailureReportsAcceptedUpdateAndSettlesWithoutReapplyingIt) {
    Stream recorded;
    SurfaceSession a;
    ASSERT_NO_THROW(startCatchmentProvider(a, "stream-update-write", 2));
    const auto path = streamPath("stream-update-write");
    ASSERT_EQ(a.engine.setEngineSourceResultStream(recorded, path.string(), false), "");
    const auto initial = recorded.cursor();
    SWMM_CouplingForcing rain{sizeof(rain), 1, 0, 1, .001, 5, 0, nullptr};
    SWMM_CouplingFrame f{sizeof(f), 1, "water", 0, nullptr, 0, nullptr, 1, &rain, 0, nullptr};
    ASSERT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&a.engine), &f), SWMM_OK);
    const auto before = recorded.cursor();
    ASSERT_EQ(before.frames, initial.frames + 2);
    const auto size = (before.bytes - initial.bytes) / 2;
    rlimit previous{};
    ASSERT_EQ(getrlimit(RLIMIT_FSIZE, &previous), 0);
    auto limited = previous;
    limited.rlim_cur = before.bytes + size + 16;
    const auto handler = std::signal(SIGXFSZ, SIG_IGN);
    ASSERT_EQ(setrlimit(RLIMIT_FSIZE, &limited), 0);
    rain.rate_m_s = .003;
    const int result = swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&a.engine), &f);
    const int restored = setrlimit(RLIMIT_FSIZE, &previous);
    std::signal(SIGXFSZ, handler);
    ASSERT_EQ(restored, 0);
    EXPECT_NE(result, SWMM_OK);
    EXPECT_TRUE(recorded.hasPendingCapture());
    EXPECT_FALSE(recorded.lastError().empty());
    const auto* accepted = a.engine.surfaceRouter2D().state().runtime_forcings.find(1, 0);
    ASSERT_NE(accepted, nullptr);
    EXPECT_DOUBLE_EQ(accepted->rate, .003);
    EXPECT_EQ(recorded.latest().kind, Stream::Kind::CouplingBarrier);
    EXPECT_EQ(recorded.cursor().frames, before.frames + 1);
    EXPECT_EQ(a.engine.step(nullptr), SWMM_ERR_LIFECYCLE);
    EXPECT_DOUBLE_EQ(a.engine.context().current_time, 0);
    ASSERT_EQ(a.engine.saveCheckpoint((root() / "stream-update-write.chk").string()), "");
    EXPECT_FALSE(recorded.hasPendingCapture());
    EXPECT_EQ(recorded.latest().kind, Stream::Kind::PrescriptionUpdate);
    EXPECT_EQ(recorded.cursor().frames, before.frames + 2);
    ASSERT_EQ(Stream::visit(path.string(), recorded.cursor(), [](const auto&) {}), "");
    ASSERT_NO_THROW(step(a.engine));
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, StreamStagingCollisionPreservesUnownedFileAndFreshStateWithValidRetry) {
    Stream recorded, resumed;
    SurfaceSession a, b;
    ASSERT_NO_THROW(startCatchmentProvider(a, "stream-staging-source", 2));
    ASSERT_NO_THROW(startCatchmentProvider(b, "stream-staging-target", 2));
    ASSERT_EQ(a.engine.setEngineSourceResultStream(
                  recorded, streamPath("stream-staging-source").string(), true),
              "");
    ASSERT_EQ(b.engine.setEngineSourceResultStream(
                  resumed, streamPath("stream-staging-target").string(), true),
              "");
    ASSERT_NO_THROW(until(a.engine, 1.4));
    const auto cp = root() / "stream-staging.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    const auto unowned = resumed.path() + ".stream-part-" + resumed.cursor().identity + "-1";
    write(unowned, "unrelated recovery file");
    const auto fresh = bytes(resumed.path());
    const auto cursor = resumed.cursor();
    EXPECT_FALSE(b.engine.loadCheckpoint(cp.string()).empty());
    EXPECT_EQ(bytes(unowned), "unrelated recovery file");
    EXPECT_DOUBLE_EQ(b.engine.context().current_time, 0);
    EXPECT_EQ(resumed.cursor(), cursor);
    EXPECT_TRUE(bytes(resumed.path()) == fresh);
    ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
    EXPECT_EQ(bytes(unowned), "unrelated recovery file");
    compareCatchmentProvider(a, b, "stream-staging-restored");
    equalStreamPrefix(recorded, resumed);
}
#endif
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
namespace {
/**
 * @brief Compute the test file-binding FNV-1a fingerprint.
 *
 * No file IO, parsing or mutation occurs; this is a reproducible binding hash, not a cryptographic
 * integrity proof.
 *
 * @param[in] data Borrowed exact binary data interpreted as unsigned bytes.
 * @return 64-bit FNV-1a value with the fixed standard offset/prime and modular unsigned overflow.
 */
uint64_t fileTestHash(const std::string& data) {
    uint64_t value = 14695981039346656037ULL;
    for (unsigned char c : data) {
        value ^= c;
        value *= 1099511628211ULL;
    }
    return value;
}
struct FileBundle {
    uint32_t version = 1;
    std::string physical, binary, mesh, source;
    /**
     * @brief Decode a file-backed session bundle into owned test-editable records.
     *
     * Reads via unchecked bytes helper, decodes physical/binary/mesh/source records and requires the
     * expected bundle magic and version 1 or 2. Throws on decode/trailing-layout/magic/version errors;
     * does not validate the captured checksum or verify referenced file contents here.
     *
     * @param[in] path Borrowed binary checkpoint filename.
     */
    explicit FileBundle(const std::filesystem::path& path) {
        detail::CompletedSourceArchive a(bytes(path));
        std::string magic;
        uint32_t sum;
        a(magic, version, sum, physical, binary, mesh, source);
        a.finish();
        if (magic != "OpenSWMM file-backed water engine session" || (version != 1 && version != 2))
            throw std::runtime_error("Not a file bundle.");
    }
    /**
     * @brief Decode the owned binary-output member file descriptor.
     *
     * Requires binary member layout version/periods/file-descriptor and archive finish. Does not check
     * the version/period count itself or open/verify the referenced file; decode errors propagate.
     * @return Owned CheckpointFile value including its copied path and fingerprint metadata.
     */
    io::CheckpointFile binaryFile() const {
        detail::CompletedSourceArchive a(binary);
        uint32_t version;
        int periods;
        std::string file;
        a(version, periods, file);
        a.finish();
        return io::CheckpointFile::decode(file);
    }
    /**
     * @brief Decode the owned mesh-output member file descriptor.
     *
     * Requires mesh member version/cadence/steps/due/GW/file layout and archive finish. Does not
     * validate these metadata fields or open/verify the referenced file; decode errors propagate.
     * @return Owned CheckpointFile value including copied path and fingerprint metadata.
     */
    io::CheckpointFile meshFile() const {
        detail::CompletedSourceArchive a(mesh);
        uint32_t version;
        double cadence, due;
        hsize_t steps;
        bool gw;
        std::string file;
        a(version, cadence, steps, due, gw, file);
        a.finish();
        return io::CheckpointFile::decode(file);
    }
    /**
     * @brief Re-encode the edited file bundle with updated record binding and payload CRC.
     *
     * Keeps physical/binary/mesh member bytes and source path/cursor/anchor, recomputes whole binding
     * over physical/binary/mesh, then recomputes bundle CRC. Requires source contract layout; does not
     * modify object members, write a file or validate member semantics. Decode/allocation exceptions
     * propagate.
     * @return Owned complete file-backed session archive bytes.
     */
    std::string encode() {
        using A = detail::CompletedSourceArchive;
        A binding;
        binding(physical, binary, mesh);
        A saved(source);
        std::string path, cursor;
        uint64_t anchor, whole;
        saved(path, cursor, anchor, whole);
        saved.finish();
        whole = fileTestHash(binding.bytes());
        A contract;
        contract(path, cursor, anchor, whole);
        auto updated = contract.bytes();
        A data;
        data(physical, binary, mesh, updated);
        auto sum = crc(data.bytes());
        std::string magic = "OpenSWMM file-backed water engine session";
        A out;
        out(magic, version, sum, physical, binary, mesh, updated);
        return out.bytes();
    }
    /**
     * @brief Refresh one member file descriptor from its current external artifact.
     *
     * Decodes selected descriptor, reads its current file size and bytes/FNV hash, then replaces
     * descriptor inside that member while preserving its other metadata. Does not rewrite the physical
     * artifact or parent bundle/source binding; caller uses encode afterward. Checked size/parse errors
     * can throw; bytes read failure has no independent status in the helper.
     *
     * @param[in] binary_file True selects the binary member; false the mesh member.
     */
    void rehash(bool binary_file) {
        auto file = binary_file ? binaryFile() : meshFile();
        file.bytes = io::checkpointFileSize(file.path);
        file.fingerprint = fileTestHash(bytes(file.path));
        using A = detail::CompletedSourceArchive;
        A out;
        auto descriptor = file.encode();
        if (binary_file) {
            A in(binary);
            uint32_t version;
            int periods;
            std::string old;
            in(version, periods, old);
            in.finish();
            out(version, periods, descriptor);
            binary = out.bytes();
        } else {
            A in(mesh);
            uint32_t version;
            double cadence, due;
            hsize_t steps;
            bool gw;
            std::string old;
            in(version, cadence, steps, due, gw, old);
            in.finish();
            out(version, cadence, steps, due, gw, descriptor);
            mesh = out.bytes();
        }
    }
};
/**
 * @brief Start a binary/mesh/source-stream file-backed continuation profile.
 *
 * Starts catchment provider with binary output, attaches the source stream to a fresh path, then
 * selects file-output checkpointing. Throws runtime_error on stream/capability rejection; partial
 * session/stream state remains owned by callers. Does not move or destroy the borrowed stream.
 *
 * @param[in,out] s Fresh owned spatial session retaining source/stage/engine lifetimes.
 * @param[in,out] stream Caller-owned source-result stream; must outlive its engine attachment.
 * @param[in] name Raw evidence stem; old source-stream file is removed and other selected artifacts
 *     may be overwritten.
 * @param[in] lid NODE LID selector forwarded to startCatchmentProvider.
 * @param[in] closure Groundwater closure selector 0..2.
 * @param[in] format Mesh-output format selector, normally nonzero for the file-output profile.
 * @param[in] detailed Request detailed source interval records in the attached stream.
 */
void startFileBundle(SurfaceSession& s, Stream& stream, const std::string& name, int lid = 2,
                     int closure = 0, int format = 2, bool detailed = true) {
    startCatchmentProvider(s, name, lid, closure, true, format);
    const auto error =
        s.engine.setEngineSourceResultStream(stream, streamPath(name).string(), detailed);
    if (!error.empty())
        throw std::runtime_error(error);
    const auto selected = s.engine.enableFileOutputCheckpointing();
    if (!selected.empty())
        throw std::runtime_error(selected);
}
}
TEST(EngineCheckpoint, FileOutputsContinuePendingBatchesAllNodeGroundwaterAndStorageProfiles) {
    for (bool detailed : {false, true})
        for (int lid : {0, 1, 2})
            for (int closure : {0, 1, 2}) {
                const int format = 1 + (lid + closure) % 4;
                const auto name = "file-outputs-" + std::to_string(detailed) + "-" +
                                  std::to_string(lid) + "-" + std::to_string(closure);
                SCOPED_TRACE(name);
                Stream recorded, resumed;
                SurfaceSession a, b;
                ASSERT_NO_THROW(startFileBundle(a, recorded, name, lid, closure, format, detailed));
                ASSERT_NO_THROW(
                    startFileBundle(b, resumed, name + "-resumed", lid, closure, format, detailed));
                ASSERT_NO_THROW(until(a.engine, 1.4));
                ASSERT_LT(a.stage.completedEnd(), a.engine.context().current_time);
                const auto cp = root() / (name + ".chk");
                ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
                const auto accepted_time = a.engine.context().current_time;
                FileBundle saved(cp);
                EXPECT_LT(bytes(cp).size(), saved.physical.size() + 16384);
                EXPECT_TRUE(std::filesystem::exists(saved.binaryFile().path));
                EXPECT_TRUE(std::filesystem::exists(saved.meshFile().path));
                const auto previous = root() / (name + "-previous.chk");
                write(previous, bytes(cp));
                double left, right;
                ASSERT_EQ(a.engine.advanceTo(25, &left), SWMM_OK);
                ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
                // A copied older envelope keeps its immutable companions and stream prefix
                // while the live HDF5 file and the latest checkpoint continue to change.
                ASSERT_NO_THROW(saved.binaryFile().verify());
                ASSERT_NO_THROW(saved.meshFile().verify());
                ASSERT_EQ(b.engine.loadCheckpoint(previous.string()), "");
                EXPECT_DOUBLE_EQ(b.engine.context().current_time, accepted_time);
                ASSERT_EQ(b.engine.advanceTo(25, &right), SWMM_OK);
                EXPECT_DOUBLE_EQ(left, right);
                compareCatchmentProvider(a, b, name + "-continued");
                equalStreamPrefix(recorded, resumed);
                ASSERT_EQ(a.engine.end(), 0);
                ASSERT_EQ(b.engine.end(), 0);
                sameBinary(name, name + "-resumed", format == 4 ? 4 : 8);
                sameMesh(name, name + "-resumed", format == 4 ? 2 : 4);
            }
}
TEST(EngineCheckpoint,
     FileOutputsDamagedCompanionsPreserveDriverPhysicsAndAllFreshOutputsWithRetry) {
    Stream recorded;
    TestWaterDriver driver;
    SurfaceSession a;
    ASSERT_NO_THROW(startCatchmentProvider(a, "file-damage-source", 2, 2, true, 2));
    ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(driver), "");
    ASSERT_EQ(a.engine.setEngineSourceResultStream(recorded,
                                                   streamPath("file-damage-source").string(), true),
              "");
    ASSERT_EQ(a.engine.enableFileOutputCheckpointing(), "");
    ASSERT_NO_THROW(drive(a.engine, driver, 4.2));
    const auto cp = root() / "file-damage.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    FileBundle bundle(cp);
    for (int variant = 0; variant < 6; ++variant) {
        const auto name = "file-damage-target-" + std::to_string(variant);
        SCOPED_TRACE(name);
        Stream target;
        TestWaterDriver target_driver;
        SurfaceSession b;
        ASSERT_NO_THROW(startCatchmentProvider(b, name, 2, 2, true, 2));
        ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(target_driver), "");
        ASSERT_EQ(b.engine.setEngineSourceResultStream(target, streamPath(name).string(), true),
                  "");
        ASSERT_EQ(b.engine.enableFileOutputCheckpointing(), "");
        const auto binary = root() / (name + ".out"), mesh = root() / (name + ".h5");
        const auto bh = bytes(binary), mh = bytes(mesh), sh = bytes(target.path());
        const auto cursor = target.cursor();
        const auto initial = driverState(target_driver, 0);
        const auto damaged = variant < 3 ? bundle.binaryFile().path : bundle.meshFile().path;
        const auto original = bytes(damaged);
        if (variant % 3 == 0)
            std::filesystem::rename(damaged, damaged.string() + ".hidden");
        else {
            auto altered = original;
            if (variant % 3 == 1)
                altered.resize(altered.size() - 1);
            else
                altered[altered.size() / 2] ^= 1;
            write(damaged, altered);
        }
        EXPECT_FALSE(b.engine.loadCheckpoint(cp.string()).empty());
        EXPECT_DOUBLE_EQ(b.engine.context().current_time, 0);
        EXPECT_EQ(bytes(binary), bh);
        EXPECT_EQ(bytes(mesh), mh);
        EXPECT_EQ(bytes(target.path()), sh);
        EXPECT_EQ(target.cursor(), cursor);
        EXPECT_EQ(driverState(target_driver, 0), initial);
        if (variant % 3 == 0)
            std::filesystem::rename(damaged.string() + ".hidden", damaged);
        else
            write(damaged, original);
        ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
        compareCatchmentProvider(a, b, name + "-restored");
        equalStreamPrefix(recorded, target);
        const auto due = target.cursor();
        double actual;
        EXPECT_EQ(b.engine.advanceTo(5, &actual), SWMM_ERR_LIFECYCLE);
        EXPECT_EQ(target.cursor(), due);
    }
}
TEST(EngineCheckpoint,
     FileOutputsFailedPublicationAndUnsupportedEmbeddedHistoryPreserveAcceptedCheckpoint) {
    Stream recorded, resumed;
    SurfaceSession a, b;
    ASSERT_NO_THROW(startFileBundle(a, recorded, "file-publish-source"));
    ASSERT_NO_THROW(startFileBundle(b, resumed, "file-publish-target"));
    ASSERT_NO_THROW(until(a.engine, 9.1));
    const auto cp = root() / "file-publish.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    const auto binary = root() / "file-publish-target.out",
               mesh = root() / "file-publish-target.h5";
    const auto bh = bytes(binary), mh = bytes(mesh), sh = bytes(resumed.path());
    std::filesystem::permissions(mesh, std::filesystem::perms::owner_read);
    EXPECT_FALSE(b.engine.loadCheckpoint(cp.string()).empty());
    EXPECT_DOUBLE_EQ(b.engine.context().current_time, 0);
    EXPECT_EQ(bytes(binary), bh);
    EXPECT_EQ(bytes(mesh), mh);
    EXPECT_EQ(bytes(resumed.path()), sh);
    std::filesystem::permissions(mesh, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write);
    ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
    compareCatchmentProvider(a, b, "file-publish-retry");
    // The new profile refuses a growing embedded legacy history rather than
    // silently reading it into an unbounded checkpoint participant.
    const auto checkpoint = bytes(cp);
    twoD::SourceResultHistory history;
    SurfaceSession unused;
    ASSERT_NO_THROW(startCatchmentProvider(unused, "file-embedded-initial", 2, 0, true, 2));
    ASSERT_EQ(history.append(unused.stage), "");
    meshWriter(a.engine).enqueueSourceHistory(std::make_shared<twoD::SourceResultHistory>(history));
    EXPECT_FALSE(a.engine.saveCheckpoint(cp.string()).empty());
    EXPECT_EQ(bytes(cp), checkpoint);
}
TEST(EngineCheckpoint, FileOutputsValidateFiniteValuesDatesAndSchemaBeyondFileIntegrity) {
    Stream recorded;
    SurfaceSession a;
    ASSERT_NO_THROW(startFileBundle(a, recorded, "file-semantic-source"));
    ASSERT_NO_THROW(until(a.engine, 9.1));
    const auto cp = root() / "file-semantic.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    for (int variant = 0; variant < 4; ++variant) {
        const auto name = "file-semantic-target-" + std::to_string(variant);
        SCOPED_TRACE(name);
        Stream target;
        SurfaceSession b;
        ASSERT_NO_THROW(startFileBundle(b, target, name));
        FileBundle changed(cp);
        const auto companion = variant == 0 ? changed.binaryFile().path : changed.meshFile().path;
        const auto original = bytes(companion);
        if (variant == 0) {
            auto damaged = original;
            const float value = std::numeric_limits<float>::quiet_NaN();
            std::memcpy(damaged.data() + damaged.size() - sizeof(value), &value, sizeof(value));
            write(companion, damaged);
        } else {
            const auto file = H5Fopen(companion.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
            ASSERT_GE(file, 0);
            const auto time = H5Dopen2(file, "time", H5P_DEFAULT);
            ASSERT_GE(time, 0);
            if (variant < 3) {
                const auto space = H5Dget_space(time);
                const auto count = H5Sget_simple_extent_npoints(space);
                ASSERT_GT(count, 0);
                std::vector<double> dates(count);
                ASSERT_GE(
                    H5Dread(time, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, dates.data()),
                    0);
                dates[0] = variant == 1 ? 0 : std::numeric_limits<double>::infinity();
                ASSERT_GE(
                    H5Dwrite(time, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, dates.data()),
                    0);
                H5Sclose(space);
            } else {
                const hsize_t wrong = 3;
                ASSERT_GE(H5Dset_extent(time, &wrong), 0);
            }
            H5Dclose(time);
            ASSERT_GE(H5Fclose(file), 0);
        }
        // Rebuild the accidental-damage checks so semantic validation is exercised.
        changed.rehash(variant == 0);
        const auto bad = root() / (name + ".chk");
        write(bad, changed.encode());
        const auto binary = root() / (name + ".out"), mesh = root() / (name + ".h5");
        const auto bh = bytes(binary), mh = bytes(mesh), sh = bytes(target.path());
        EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());
        EXPECT_DOUBLE_EQ(b.engine.context().current_time, 0);
        EXPECT_EQ(bytes(binary), bh);
        EXPECT_EQ(bytes(mesh), mh);
        EXPECT_EQ(bytes(target.path()), sh);
        write(companion, original);
        ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
        compareCatchmentProvider(a, b, name + "-retry");
        equalStreamPrefix(recorded, target);
    }
}
TEST(EngineCheckpoint,
     FileOutputsRequireExplicitFreshCombinedStreamProfileAndSupportInitialCheckpoint) {
    Stream recorded;
    SurfaceSession a;
    ASSERT_NO_THROW(startCatchmentProvider(a, "file-profile-source", 2, 0, true, 2));
    EXPECT_FALSE(a.engine.enableFileOutputCheckpointing().empty());
    ASSERT_EQ(a.engine.setEngineSourceResultStream(
                  recorded, streamPath("file-profile-source").string(), true),
              "");
    ASSERT_EQ(a.engine.enableFileOutputCheckpointing(), "");
    EXPECT_FALSE(a.engine.enableFileOutputCheckpointing().empty());
    const auto cp = root() / "file-profile.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    Stream target;
    SurfaceSession b;
    ASSERT_NO_THROW(startCatchmentProvider(b, "file-profile-target", 2, 0, true, 2));
    ASSERT_EQ(b.engine.setEngineSourceResultStream(
                  target, streamPath("file-profile-target").string(), true),
              "");
    const auto fresh = bytes(target.path());
    EXPECT_FALSE(b.engine.loadCheckpoint(cp.string()).empty());
    EXPECT_EQ(bytes(target.path()), fresh);
    ASSERT_EQ(b.engine.enableFileOutputCheckpointing(), "");
    ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
    equalStreamPrefix(recorded, target);
    ASSERT_NO_THROW(step(a.engine));
    ASSERT_NO_THROW(step(b.engine));
    compareCatchmentProvider(a, b, "file-profile-continued");
    equalStreamPrefix(recorded, target);
    Stream late;
    SurfaceSession c;
    ASSERT_NO_THROW(startCatchmentProvider(c, "file-profile-late", 2, 0, true, 2));
    ASSERT_EQ(
        c.engine.setEngineSourceResultStream(late, streamPath("file-profile-late").string(), true),
        "");
    ASSERT_NO_THROW(step(c.engine));
    EXPECT_FALSE(c.engine.enableFileOutputCheckpointing().empty());
}
TEST(EngineCheckpoint, FileOutputCopiesValidateLargePrefixesAndNeverDeleteUnownedStagingFiles) {
    const auto source = root() / "file-large-prefix.bin";
    FILE* file = io::fopen_path(source, "wb");
    ASSERT_NE(file, nullptr);
    std::array<char, 65536> block{};
    block[7] = 42;
    for (int i = 0; i < 1280; ++i)
        ASSERT_EQ(std::fwrite(block.data(), 1, block.size(), file), block.size());
    ASSERT_EQ(std::fclose(file), 0);
    auto first =
        io::copyCheckpointFile(source, io::checkpointFileSize(source), root() / "file-large-copy");
    EXPECT_EQ(first->file.bytes, 80 * 1024 * 1024ULL);
    ASSERT_NO_THROW(first->file.verify());
    auto second =
        io::copyCheckpointFile(first->file.path, first->file.bytes, root() / "file-large-resumed");
    EXPECT_EQ(second->file.fingerprint, first->file.fingerprint);
    // Every candidate staging name collides with an existing recovery artifact.
    // Exclusive creation must leave all these files intact on failure.
    const auto name = second->file.path.filename().string();
    const auto next = std::stoull(name.substr(name.rfind(".file-part-") + 11)) + 1;
    const auto base = root() / "file-unowned-staging";
    for (uint64_t i = next; i < next + 32; ++i)
        write(base.string() + ".file-part-" + std::to_string(i), "recovery");
    EXPECT_THROW(io::copyCheckpointFile(source, first->file.bytes, base), std::runtime_error);
    for (uint64_t i = next; i < next + 32; ++i) {
        auto path = base.string() + ".file-part-" + std::to_string(i);
        EXPECT_EQ(bytes(path), "recovery");
        std::filesystem::remove(path);
    }
}

namespace {
/**
 * @brief Rewrite one file-bundle member path without touching its physical artifact.
 *
 * Preserves member cursor/count/cadence/hash metadata and decodes exact known wire layouts. Does
 * not rename/move a file, verify the new destination, rehash contents or recompute the parent
 * bundle checksum; caller uses encode afterward. Decode/path/allocation errors propagate.
 *
 * @param[in,out] bundle Decoded mutable FileBundle; selected member wire bytes are replaced.
 * @param[in] member 0 binary, 2 source stream, other values mesh output.
 * @param[in] path Replacement UTF-8 path string; copied into source record or converted to a
 *     filesystem descriptor path.
 */
void changeBundlePath(FileBundle& bundle, int member, const std::string& path) {
    using A = detail::CompletedSourceArchive;
    A out;
    if (member == 2) {
        A in(bundle.source);
        std::string old, cursor;
        uint64_t anchor, whole;
        in(old, cursor, anchor, whole);
        in.finish();
        auto updated = path;
        out(updated, cursor, anchor, whole);
        bundle.source = out.bytes();
        return;
    }
    auto file = member == 0 ? bundle.binaryFile() : bundle.meshFile();
    file.path = io::utf8_path(path);
    auto descriptor = file.encode();
    if (member == 0) {
        A in(bundle.binary);
        uint32_t version;
        int periods;
        std::string old;
        in(version, periods, old);
        in.finish();
        out(version, periods, descriptor);
        bundle.binary = out.bytes();
    } else {
        A in(bundle.mesh);
        uint32_t version;
        double cadence, due;
        hsize_t steps;
        bool gw;
        std::string old;
        in(version, cadence, steps, due, gw, old);
        in.finish();
        out(version, cadence, steps, due, gw, descriptor);
        bundle.mesh = out.bytes();
    }
}
}
TEST(EngineCheckpoint, BundleRelocationContinuesSavedPrefixesAcrossNodeAndGroundwaterProfiles) {
    for (bool detailed : {false, true})
        for (int lid : {0, 1, 2})
            for (int closure : {0, 1, 2}) {
                const auto name = "bundle-profiles-" + std::to_string(detailed) + "-" +
                                  std::to_string(lid) + "-" + std::to_string(closure);
                SCOPED_TRACE(name);
                const int format = 1 + (lid + closure) % 4;
                Stream recorded, resumed;
                SurfaceSession a, b;
                ASSERT_NO_THROW(startFileBundle(a, recorded, name, lid, closure, format, detailed));
                ASSERT_NO_THROW(
                    startFileBundle(b, resumed, name + "-resumed", lid, closure, format, detailed));
                const double phase = (lid + closure) % 3 == 0   ? 0
                                     : (lid + closure) % 3 == 1 ? 1.4
                                                                : 9.1;
                if (phase > 0)
                    ASSERT_NO_THROW(until(a.engine, phase));
                const auto cp = root() / (name + ".chk");
                ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
                const auto cursor = recorded.cursor();
                const double time = a.engine.context().current_time;
                // Package the older accepted checkpoint after the live journal has advanced.
                double actual;
                ASSERT_EQ(a.engine.advanceTo(25, &actual), SWMM_OK);
                ASSERT_GT(recorded.cursor().bytes, cursor.bytes);
                const auto folder = root() / (name + " export λ");
                ASSERT_EQ(SWMMEngine::packageCheckpointBundle(cp.string(), io::path_utf8(folder)),
                          "");
                FileBundle bundle(folder / "session.chk");
                EXPECT_EQ(bundle.version, 2);
                EXPECT_EQ(bundle.binaryFile().path, "binary.out");
                EXPECT_EQ(bundle.meshFile().path, "mesh.h5");
                EXPECT_EQ(bytes(folder / "source.history"),
                          bytes(recorded.path()).substr(0, cursor.bytes));
                EXPECT_EQ(std::distance(std::filesystem::directory_iterator(folder),
                                        std::filesystem::directory_iterator{}),
                          4);
                const auto moved = root() / (name + " moved λ");
                std::filesystem::rename(folder, moved);
                ASSERT_EQ(b.engine.loadCheckpoint(io::path_utf8(moved / "session.chk")), "");
                EXPECT_DOUBLE_EQ(b.engine.context().current_time, time);
                EXPECT_EQ(resumed.cursor(), cursor);
                ASSERT_EQ(b.engine.advanceTo(25, &actual), SWMM_OK);
                compareCatchmentProvider(a, b, name + "-continued");
                equalStreamPrefix(recorded, resumed);
                ASSERT_EQ(a.engine.end(), 0);
                ASSERT_EQ(b.engine.end(), 0);
                sameBinary(name, name + "-resumed", format == 4 ? 4 : 8);
                sameMesh(name, name + "-resumed", format == 4 ? 2 : 4);
            }
}
TEST(EngineCheckpoint,
     BundleRepackagesAfterOriginalFilesDisappearAndRestoresDueDriverWithoutMutation) {
    Stream recorded;
    TestWaterDriver driver;
    SurfaceSession a;
    ASSERT_NO_THROW(startCatchmentProvider(a, "bundle-detached-source", 2, 2, true, 2));
    ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(driver), "");
    ASSERT_EQ(a.engine.setEngineSourceResultStream(
                  recorded, streamPath("bundle-detached-source").string(), true),
              "");
    ASSERT_EQ(a.engine.enableFileOutputCheckpointing(), "");
    ASSERT_NO_THROW(drive(a.engine, driver, 4.2));
    const auto cp = root() / "bundle-detached.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    const auto cursor = recorded.cursor();
    const auto state = driverState(driver, a.engine.context().current_time);
    FileBundle original(cp);
    const auto folder = root() / "bundle-detached-export";
    ASSERT_EQ(SWMMEngine::packageCheckpointBundle(cp.string(), folder.string()), "");
    ASSERT_EQ(a.engine.end(), 0);
    ASSERT_EQ(a.engine.close(), 0);
    for (const auto& file :
         {cp, original.binaryFile().path, original.meshFile().path, io::utf8_path(recorded.path())})
        ASSERT_TRUE(std::filesystem::remove(file));
    const auto moved = root() / "bundle-detached-moved";
    std::filesystem::rename(folder, moved);
    write(moved / "unrelated.txt", "keep");
    const auto repacked = root() / "bundle-detached-repacked";
    ASSERT_EQ(
        SWMMEngine::packageCheckpointBundle((moved / "session.chk").string(), repacked.string()),
        "");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(repacked),
                            std::filesystem::directory_iterator{}),
              4);
    EXPECT_EQ(bytes(moved / "unrelated.txt"), "keep");
    std::filesystem::rename(moved, root() / "bundle-detached-no-longer-needed");
    Stream resumed;
    TestWaterDriver target;
    SurfaceSession b;
    ASSERT_NO_THROW(startCatchmentProvider(b, "bundle-detached-target", 2, 2, true, 2));
    ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(target), "");
    ASSERT_EQ(b.engine.setEngineSourceResultStream(
                  resumed, streamPath("bundle-detached-target").string(), true),
              "");
    ASSERT_EQ(b.engine.enableFileOutputCheckpointing(), "");
    ASSERT_EQ(b.engine.loadCheckpoint((repacked / "session.chk").string()), "");
    EXPECT_EQ(resumed.cursor(), cursor);
    EXPECT_EQ(driverState(target, b.engine.context().current_time), state);
    const auto fresh = bytes(resumed.path());
    const auto binary = bytes(root() / "bundle-detached-target.out"),
               mesh = bytes(root() / "bundle-detached-target.h5");
    double actual;
    EXPECT_EQ(b.engine.advanceTo(5, &actual), SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(bytes(resumed.path()), fresh);
    EXPECT_EQ(bytes(root() / "bundle-detached-target.out"), binary);
    EXPECT_EQ(bytes(root() / "bundle-detached-target.h5"), mesh);
    ASSERT_NO_THROW(drive(b.engine, target, 25));
    EXPECT_EQ(target.cursor, 5);
    EXPECT_EQ(target.nextUpdateSeconds(), -1);
}
TEST(EngineCheckpoint, BundleDamagedAndEscapingMembersPreserveFreshOutputsAndAllowRetry) {
    Stream recorded;
    SurfaceSession a;
    ASSERT_NO_THROW(startFileBundle(a, recorded, "bundle-damage-source"));
    ASSERT_NO_THROW(until(a.engine, 9.1));
    const auto cp = root() / "bundle-damage.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    const auto folder = root() / "bundle-damage-export";
    ASSERT_EQ(SWMMEngine::packageCheckpointBundle(cp.string(), folder.string()), "");
    const auto manifest = bytes(folder / "session.chk");
    for (int variant = 0; variant < 11; ++variant) {
        const auto name = "bundle-damage-target-" + std::to_string(variant);
        SCOPED_TRACE(name);
        Stream resumed;
        SurfaceSession b;
        ASSERT_NO_THROW(startFileBundle(b, resumed, name));
        auto input = folder / "session.chk";
        std::filesystem::path altered;
        std::string original;
        FileBundle bad(input);
        if (variant < 4) {
            altered = folder / (variant < 2    ? "source.history"
                                : variant == 2 ? "binary.out"
                                               : "mesh.h5");
            original = bytes(altered);
            if (variant == 0)
                std::filesystem::rename(altered, altered.string() + ".missing");
            else {
                auto data = original;
                if (variant == 1)
                    data += "extra tail";
                else if (variant == 2)
                    data[data.size() / 2] ^= 1;
                else
                    data.resize(data.size() - 1);
                write(altered, data);
            }
        } else if (variant < 8) {
            if (variant == 4)
                changeBundlePath(bad, 0, "../binary.out");
            if (variant == 5)
                changeBundlePath(bad, 1, io::path_utf8(FileBundle(cp).meshFile().path));
            if (variant == 6)
                changeBundlePath(bad, 2, "../source.history");
            if (variant == 7)
                changeBundlePath(bad, 0, "renamed.out");
            write(input, bad.encode());
        } else if (variant == 8) {
#ifndef _WIN32
            altered = folder / "source.history";
            std::filesystem::rename(altered, altered.string() + ".original");
            std::filesystem::create_symlink(altered.string() + ".original", altered);
#else
            continue;
#endif
        } else if (variant == 9) {
#ifndef _WIN32
            input = folder / "linked.chk";
            std::filesystem::create_symlink(folder / "session.chk", input);
#else
            continue;
#endif
        } else
            std::filesystem::permissions(root() / (name + ".h5"),
                                         std::filesystem::perms::owner_read);
        const auto binary = root() / (name + ".out"), mesh = root() / (name + ".h5");
        const auto bh = bytes(binary), mh = bytes(mesh), sh = bytes(resumed.path());
        const auto cursor = resumed.cursor();
        EXPECT_FALSE(b.engine.loadCheckpoint(input.string()).empty());
        EXPECT_DOUBLE_EQ(b.engine.context().current_time, 0);
        EXPECT_EQ(bytes(binary), bh);
        EXPECT_EQ(bytes(mesh), mh);
        EXPECT_EQ(bytes(resumed.path()), sh);
        EXPECT_EQ(resumed.cursor(), cursor);
        if (variant == 0)
            std::filesystem::rename(altered.string() + ".missing", altered);
        else if (variant < 4)
            write(altered, original);
        else if (variant < 8)
            write(folder / "session.chk", manifest);
        else if (variant == 8) {
            std::filesystem::remove(altered);
            std::filesystem::rename(altered.string() + ".original", altered);
        } else if (variant == 9)
            std::filesystem::remove(input);
        else
            std::filesystem::permissions(mesh, std::filesystem::perms::owner_read |
                                                   std::filesystem::perms::owner_write);
        ASSERT_EQ(b.engine.loadCheckpoint((folder / "session.chk").string()), "");
        compareCatchmentProvider(a, b, name + "-retry");
        equalStreamPrefix(recorded, resumed);
    }
}
TEST(EngineCheckpoint, BundlePackagingRejectsBrokenBindingsAndPreservesDestinationsAndSourceState) {
    Stream recorded;
    SurfaceSession a;
    ASSERT_NO_THROW(startFileBundle(a, recorded, "bundle-package-guards"));
    ASSERT_NO_THROW(until(a.engine, 9.1));
    const auto cp = root() / "bundle-package-guards.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    const auto before = bytes(cp), source = bytes(recorded.path());
    const auto cursor = recorded.cursor();
    const auto time = a.engine.context().current_time;
    for (int variant = 0; variant < 5; ++variant) {
        const auto destination = root() / ("bundle-package-invalid-" + std::to_string(variant));
        FileBundle bad(cp);
        if (variant == 0) {
            detail::CompletedSourceArchive in(bad.source);
            std::string path, saved;
            uint64_t anchor, whole;
            in(path, saved, anchor, whole);
            ++anchor;
            detail::CompletedSourceArchive out;
            out(path, saved, anchor, whole);
            bad.source = out.bytes();
        } else if (variant == 1) {
            auto f = bad.binaryFile();
            f.fingerprint ^= 1;
            detail::CompletedSourceArchive in(bad.binary);
            uint32_t version;
            int periods;
            std::string old;
            in(version, periods, old);
            auto descriptor = f.encode();
            detail::CompletedSourceArchive out;
            out(version, periods, descriptor);
            bad.binary = out.bytes();
        } else if (variant == 2) {
            bad.physical.back() ^= 1;
        }
        const auto input = root() / ("bundle-package-bad-" + std::to_string(variant) + ".chk");
        auto data = bad.encode();
        if (variant == 3)
            data.back() ^= 1;
        write(input, data);
        if (variant == 4) {
            std::filesystem::create_directory(destination);
            write(destination / "recovery", "unrelated");
        }
        EXPECT_FALSE(
            SWMMEngine::packageCheckpointBundle(input.string(), destination.string()).empty());
        if (variant == 4)
            EXPECT_EQ(bytes(destination / "recovery"), "unrelated");
        else
            EXPECT_FALSE(std::filesystem::exists(destination));
        EXPECT_EQ(bytes(cp), before);
        EXPECT_EQ(bytes(recorded.path()), source);
        EXPECT_EQ(recorded.cursor(), cursor);
        EXPECT_DOUBLE_EQ(a.engine.context().current_time, time);
    }
    const auto file = root() / "bundle-unrelated-destination";
    write(file, "unrelated");
    EXPECT_FALSE(SWMMEngine::packageCheckpointBundle(cp.string(), file.string()).empty());
    EXPECT_EQ(bytes(file), "unrelated");
    const auto retry = root() / "bundle-package-retry";
    ASSERT_EQ(SWMMEngine::packageCheckpointBundle(cp.string(), retry.string()), "");
}
TEST(EngineCheckpoint, BundleDirectoryCollisionsAndConcurrentDestinationNeverRemoveUnownedFiles) {
    const auto first = root() / "bundle-directory-first";
    uint64_t next;
    {
        io::CheckpointDirectory stage(first);
        const auto name = stage.staging().filename().string();
        next = std::stoull(name.substr(name.rfind(".bundle-part-") + 13)) + 1;
    }
    const auto destination = root() / "bundle-directory-collisions";
    for (uint64_t k = next; k < next + 32; ++k) {
        auto path =
            std::filesystem::path(destination.string() + ".bundle-part-" + std::to_string(k));
        std::filesystem::create_directory(path);
        write(path / "recovery", "unowned");
    }
    EXPECT_THROW(io::CheckpointDirectory stage(destination), std::runtime_error);
    for (uint64_t k = next; k < next + 32; ++k) {
        auto path =
            std::filesystem::path(destination.string() + ".bundle-part-" + std::to_string(k));
        EXPECT_EQ(bytes(path / "recovery"), "unowned");
        std::filesystem::remove_all(path);
    }
    const auto raced = root() / "bundle-directory-raced";
    std::filesystem::path private_folder;
    {
        io::CheckpointDirectory stage(raced);
        private_folder = stage.staging();
        write(private_folder / "session.chk", "owned");
        std::filesystem::create_directory(raced);
        EXPECT_THROW(stage.publish(), std::runtime_error);
        EXPECT_TRUE(std::filesystem::is_directory(raced));
        EXPECT_TRUE(std::filesystem::is_empty(raced));
    }
    EXPECT_FALSE(std::filesystem::exists(private_folder));
    EXPECT_TRUE(std::filesystem::is_directory(raced));
}
#endif

#ifdef OPENSWMM_HAS_2D
namespace {
// Branching runoff graphs feed storage nodes on a connected sloping grid.
// Alternate high/low outfall stages exercise porous-cell backwater and drainage.
/**
 * @brief Start a scaled connected-grid/source-graph spatial checkpoint profile.
 *
 * Rejects explicit dimension constraints, then authors twelve-second CFS branching source network
 * with alternating backwater/drainage, native LIDs and a connected SI triangle grid. Maps initial
 * cells to storages, installs reviewed 0.05 m2 footprints across all mesh cells and enables spatial
 * checkpointing. Some derived bounds (positive per-LID stride and integer overflow) remain caller
 * preconditions. Raw writes are unchecked; lifecycle/output/source errors throw with partial owned
 * state retained. No iteration/size guard protects excessively large valid counts.
 *
 * @param[in,out] s Fresh owned session retaining source/stage/engine lifetimes.
 * @param[in] name Raw root-relative evidence stem; selected input/report/result artifacts may be
 *     overwritten.
 * @param[in] side Grid side in metres/cell count; at least 2, and squared counts must fit
 *     int/vector sizes.
 * @param[in] nodes Storage/outfall/conduit group count in [2,2*side*side].
 * @param[in] sources Source count divisible by nodes and graphs, at least 2*graphs and at least
 *     nodes so per-LID iteration stride is positive.
 * @param[in] lid 0 ordinary storages, nonzero NODE LIDs, exact 2 Richards layer flow.
 * @param[in] closure 2 SIGMA, nonzero ENSLAVED otherwise CLOSED_FORM.
 * @param[in] output Prepare combined binary/mesh checkpoint output and start saved results when
 *     true.
 * @param[in] graphs Zero defaults to nodes; otherwise source routing-graph count in [1,nodes],
 *     dividing sources.
 */
void startScaledSurface(SurfaceSession& s, const std::string& name, int side, int nodes,
                        int sources, int lid = 2, int closure = 0, bool output = false,
                        int graphs = 0) {
    if (!graphs)
        graphs = nodes;
    if (side < 2 || nodes < 2 || nodes > 2 * side * side || graphs < 1 || graphs > nodes ||
        sources % nodes || sources % graphs || sources < graphs * 2)
        throw std::runtime_error("Invalid scale fixture dimensions");
    const auto path = root() / name;
    const int per_graph = sources / graphs, cells = 2 * side * side;
    std::string deck =
        "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 10/12/2026\nEND_DATE 10/12/2026\nEND_TIME 00:00:12\nROUTING_STEP .2\nVARIABLE_STEP 0\nREPORT_STEP 00:00:01\nWET_STEP 00:00:01\nDRY_STEP 00:00:03\nINFILTRATION GREEN_AMPT\nIGNORE_QUALITY YES\nIGNORE_SNOWMELT YES\nIGNORE_GROUNDWATER YES\nIGNORE_2D NO\n";
    auto id = [](const char* prefix, int i) { return std::string(prefix) + std::to_string(i); };
    deck += "[STORAGE]\n";
    for (int i = 0; i < nodes; ++i)
        deck += id("N", i) + " 0 2 1.4 FUNCTIONAL 0 0 100 0 0\n";
    deck += "[OUTFALLS]\n";
    for (int i = 0; i < nodes; ++i)
        deck += id("O", i) + " 0 FIXED " + (i % 2 ? "2.5" : ".4") + " NO\n";
    deck += "[CONDUITS]\n";
    for (int i = 0; i < nodes; ++i)
        deck += id("C", i) + " " + id("N", i) + " " + id("O", i) + " 10 .013 .75 0 0 0\n";
    deck += "[XSECTIONS]\n";
    for (int i = 0; i < nodes; ++i)
        deck += id("C", i) + " CIRCULAR .1 0 0 0\n";
    deck += "[RAINGAGES]\nR INTENSITY 0:00:02 1 TIMESERIES Rain\n[SUBCATCHMENTS]\n";
    for (int i = 0; i < sources; ++i) {
        const int branch = i % per_graph, group = i / per_graph;
        const auto outlet = branch ? id("A", group * per_graph + (branch - 1) / 2)
                                   : id("N", group * nodes / graphs);
        deck +=
            id("A", i) + " R " + outlet + " .001 " + std::to_string(30 + i % 5 * 10) + " 5 1 0\n";
    }
    deck += "[SUBAREAS]\n";
    for (int i = 0; i < sources; ++i)
        deck += id("A", i) + " .015 .12 .03 .05 35 OUTLET\n";
    deck += "[INFILTRATION]\n";
    for (int i = 0; i < sources; ++i)
        deck += id("A", i) + " 3 .25 .3\n";
    deck +=
        "[LID_CONTROLS]\nI IT\nI SURFACE 3 .05 .12 1 3\nI STORAGE 6 .667 .1 0\nI DRAIN .5 .5 0 0\n[LID_USAGE]\n";
    const int per_lid = sources / nodes;
    for (int i = 0; i < sources; i += per_lid)
        deck += id("A", i) + " I 1 2 1 50 30 0 * " + id("N", i / per_lid) + "\n";
    if (lid) {
        deck +=
            "[LID_CONTROLS]\nStack NODE\nStack SURFACE 6 .1\nStack MEDIA 12 .45 .2 .08 2 10 3\nStack AGGREGATE 6 .4 100\nStack BOTTOM .5 0\n[LID_NODES]\n";
        for (int i = 0; i < nodes; ++i)
            deck += id("N", i) + " Stack " + std::to_string(10 + i % 3) + "\n";
        if (lid == 2)
            deck +=
                "[LID_RICHARDS]\nStack OPTIONS 4 1e-7 1e-5 30\nStack 2 .03 2 1.6 .5 .0001\nStack 3 .03 2 1.6 .5 .0001\n";
    }
    deck +=
        "[EVAPORATION]\nCONSTANT .1\n[TIMESERIES]\nRain 10/12/2026 00:00:00 5\nRain 10/12/2026 00:00:02 0\nRain 10/12/2026 00:00:04 3\nRain 10/12/2026 00:00:06 0\n";
    if (output)
        deck += "[2D_OPTIONS]\nOUTPUT_FILE " + path.string() + ".h5\nREPORT_2D_STEP 00:00:01\n";
    write(path.string() + ".inp", deck);
    auto& e = s.engine;
    if (e.open((path.string() + ".inp").c_str(), (path.string() + ".rpt").c_str(),
               output ? (path.string() + ".out").c_str() : nullptr))
        throw std::runtime_error(e.context().error_message);
    auto& router = e.surfaceRouter2D();
    auto& mesh = router.mesh();
    mesh.resize_vertices((side + 1) * (side + 1));
    mesh.resize_triangles(cells);
    for (int y = 0; y <= side; ++y)
        for (int x = 0; x <= side; ++x) {
            const int v = y * (side + 1) + x;
            mesh.vx[v] = x;
            mesh.vy[v] = y;
            mesh.vz[v] = .5 + .001 * x;
        }
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            const int a = y * (side + 1) + x, k = 2 * (y * side + x);
            mesh.set_triangle(k, a, a + 1, a + side + 1);
            mesh.set_triangle(k + 1, a + 1, a + side + 2, a + side + 1);
        }
    for (int i = 0; i < cells; ++i) {
        mesh.mannings_n[i] = .03;
        mesh.tri_init_depth[i] = .02 + .002 * (i % 5);
        mesh.tri_coupled_node[i] = i < nodes ? e.context().node_names.find(id("N", i)) : -1;
        mesh.tri_coupling_cd[i] = .6;
        mesh.tri_coupling_area[i] = .0002;
    }
    auto& options = router.options();
    options.mesh_units_si = true;
    options.num_threads = 1;
    options.momentum = twoD::Momentum2D::LOCAL_INERTIAL;
    options.max_timestep = .1;
    options.coupling_sync = .7;
    options.evaporation = 2;
    options.rainfall_mode = twoD::RainfallMode::SYSTEM;
    auto& config = router.aquiferConfig();
    config.options.authored = true;
    config.options.force_closed_form = true;
    config.options.gw_et = "BOUNDARY_ET";
    config.options.closure = closure == 2 ? twoD::GwClosure::SIGMA
                             : closure    ? twoD::GwClosure::ENSLAVED
                                          : twoD::GwClosure::CLOSED_FORM;
    twoD::GwAquiferRow row;
    row.Ks = 1.e-6 * 3600 / .0254;
    row.zs = 2 / .3048;
    row.hg0 = 1 / .3048;
    row.alpha = 2 * .3048;
    config.rows.push_back(row);
    e.context().options.num_threads = 1;
    if (e.initialize())
        throw std::runtime_error(e.context().error_message);
    if (output) {
        auto error = e.prepareCombinedOutputCheckpointing();
        if (!error.empty())
            throw std::runtime_error(error);
    }
    if (e.start(output ? 1 : 0))
        throw std::runtime_error(e.context().error_message);
    std::vector<int> seed(sources);
    std::iota(seed.begin(), seed.end(), 0);
    auto error = s.sources.initialize(e.context(), seed);
    if (!error.empty())
        throw std::runtime_error(error);
    twoD::SurfaceOwnershipPreview preview;
    preview.mesh_weather_area = mesh.tri_area;
    constexpr double ft2 = .3048 * .3048, inside = .05;
    for (int sc = 0; sc < sources; ++sc) {
        const auto& r = s.sources.runoff().soa();
        const double lids = s.sources.context().subcatches.total_lid_area_ft2[sc] * ft2,
                     full = r.area[sc] * ft2 + lids;
        twoD::SurfaceOwnerObject owner;
        owner.subcatch = sc;
        owner.reviewed = true;
        owner.declared_area = full;
        owner.inside_area = inside;
        owner.outside_area = full - inside;
        preview.objects.push_back(owner);
        // The current joint checkpoint profile manages every mesh cell.
        // Spread each reviewed inside footprint over its assigned grid cells.
        const int contacts = sources < cells ? 1 + (cells - 1 - sc) / sources : 1;
        for (int k = 0; k < contacts; ++k) {
            const int cell = (sc + k * sources) % cells;
            const double area = inside / contacts;
            preview.shares.push_back(
                {sc, cell, area, area * r.area[sc] * (1 - r.imperv_pct[sc]) * ft2 / full,
                 area * r.area[sc] * r.imperv_pct[sc] * ft2 / full, area * lids / full, 0});
            preview.mesh_weather_area[cell] -= area;
        }
    }
    error =
        s.stage.initialize(mesh, router.state(), options, router.subsurface(), s.sources, preview);
    if (error.empty())
        error = e.enableSpatialSourceCheckpointing(s.stage);
    if (!error.empty())
        throw std::runtime_error(error);
}
/**
 * @brief Assert selected water, Richards and network-flow invariants of the scaled fixture.
 *
 * Nonfatally checks source intervals/rain, each source-clock residual, aquifer residual, finite
 * nonnegative surface volumes and both positive/negative link flow. Requires a genuinely connected
 * mesh by neighbor-count gate. Uses fixed native/SI tolerance conventions (source 1e-6, GW 1e-8,
 * Richards 1e-9); does not compute a full cross-domain global balance or repair failures.
 *
 * @param[in] s Borrowed mutable-reference session inspected without stepping or intended state
 *     changes.
 * @param[in] lid Exact 2 additionally requires successful accepted Richards steps and tight water
 *     balance.
 */
void scaledBalance(SurfaceSession& s, int lid) {
    EXPECT_GT(s.stage.intervals(), 0);
    EXPECT_GT(s.stage.meshRain(), 0);
    for (int g = 0; g < int(s.sources.clocks().groups().size()); ++g)
        EXPECT_NEAR(s.sources.balanceResidual(g), 0, 1.e-6) << g;
    EXPECT_NEAR(s.engine.surfaceRouter2D().subsurface().state().continuityResidual(), 0, 1.e-8);
    const auto& states = s.engine.context().node_subtypes.storages.lid_state;
    if (lid == 2)
        for (const auto& state : states) {
            EXPECT_TRUE(state.richards_report.ok) << state.richards_report.error;
            EXPECT_GT(state.richards_report.accepted, 0);
            EXPECT_NEAR(state.richards_report.balance, 0, 1.e-9);
        }
    const auto& surface = s.engine.surfaceRouter2D().state();
    for (double volume : surface.volume) {
        EXPECT_TRUE(std::isfinite(volume));
        EXPECT_GE(volume, 0);
    }
    bool drainage = false, backwater = false;
    for (double flow : s.engine.context().links.flow) {
        drainage |= flow > 1.e-6;
        backwater |= flow < -1.e-6;
    }
    EXPECT_TRUE(drainage);
    EXPECT_TRUE(backwater);
    const auto& mesh = s.engine.surfaceRouter2D().mesh();
    EXPECT_GT(
        std::count_if(mesh.cell_nbr.begin(), mesh.cell_nbr.end(), [](int n) { return n >= 0; }),
        mesh.n_cells());
}
}
TEST(EngineCheckpoint, ConnectedMeshBranchingSourcesAndNodeBatchesRestartAcrossProfiles) {
    for (int lid : {0, 1, 2})
        for (int closure : {0, 1, 2}) {
            const auto name =
                "scale-profiles-" + std::to_string(lid) + "-" + std::to_string(closure);
            SCOPED_TRACE(name);
            Stream recorded, resumed;
            SurfaceSession a, b;
            ASSERT_NO_THROW(startScaledSurface(a, name, 4, 4, 16, lid, closure));
            ASSERT_NO_THROW(startScaledSurface(b, name + "-resumed", 4, 4, 16, lid, closure));
            ASSERT_EQ(
                a.engine.setEngineSourceResultStream(recorded, streamPath(name).string(), true),
                "");
            ASSERT_EQ(b.engine.setEngineSourceResultStream(
                          resumed, streamPath(name + "-resumed").string(), true),
                      "");
            ASSERT_NO_THROW(until(a.engine, 2.2));
            const auto cp = root() / (name + ".chk");
            ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
            ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
            compare(a.engine, b.engine, name + "-restored");
            equalStreamPrefix(recorded, resumed);
            ASSERT_NO_THROW(until(a.engine, 6.4));
            ASSERT_NO_THROW(until(b.engine, 6.4));
            compare(a.engine, b.engine, name + "-continued");
            equalStreamPrefix(recorded, resumed);
            EXPECT_EQ(a.sources.runoff().soa().depth_perv, b.sources.runoff().soa().depth_perv);
            ASSERT_EQ(a.sources.clocks().groups().size(), 4);
            for (const auto& group : a.sources.clocks().groups())
                EXPECT_EQ(group.subcatches.size(), 4);
            scaledBalance(a, lid);
            scaledBalance(b, lid);
            EXPECT_GT(a.engine.context().mass_balance.routing_outflow, 0);
            ASSERT_EQ(a.engine.end(), 0);
            ASSERT_EQ(b.engine.end(), 0);
            EXPECT_NEAR(a.engine.context().mass_balance_2d.error(), 0, 1.e-7);
            EXPECT_NEAR(b.engine.context().mass_balance_2d.error(), 0, 1.e-7);
            EXPECT_NEAR(a.engine.context().mass_balance.routing_error(), 0, 1.e-5);
            EXPECT_NEAR(b.engine.context().mass_balance.routing_error(), 0, 1.e-5);
        }
}
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
TEST(EngineCheckpoint, LargerConnectedSessionRelocatesAllHistoriesAndRichardsNodeStores) {
    const std::string name = "scale-large-bundle";
    Stream recorded, resumed;
    SurfaceSession a, b;
    ASSERT_NO_THROW(startScaledSurface(a, name, 12, 32, 128, 2, 0, true, 1));
    ASSERT_NO_THROW(startScaledSurface(b, name + "-resumed", 12, 32, 128, 2, 0, true, 1));
    ASSERT_EQ(a.engine.setEngineSourceResultStream(recorded, streamPath(name).string(), true), "");
    ASSERT_EQ(
        b.engine.setEngineSourceResultStream(resumed, streamPath(name + "-resumed").string(), true),
        "");
    ASSERT_EQ(a.engine.enableFileOutputCheckpointing(), "");
    ASSERT_EQ(b.engine.enableFileOutputCheckpointing(), "");
    ASSERT_NO_THROW(until(a.engine, 2.2));
    const auto cp = root() / (name + ".chk");
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    const auto folder = root() / (name + "-export"), moved = root() / (name + "-moved");
    ASSERT_EQ(SWMMEngine::packageCheckpointBundle(cp.string(), folder.string()), "");
    std::filesystem::rename(folder, moved);
    ASSERT_EQ(b.engine.loadCheckpoint((moved / "session.chk").string()), "");
    ASSERT_NO_THROW(until(a.engine, 6.4));
    ASSERT_NO_THROW(until(b.engine, 6.4));
    compare(a.engine, b.engine, name);
    equalStreamPrefix(recorded, resumed);
    EXPECT_EQ(a.engine.surfaceRouter2D().state().volume, b.engine.surfaceRouter2D().state().volume);
    EXPECT_EQ(a.sources.runoff().soa().depth_perv, b.sources.runoff().soa().depth_perv);
    ASSERT_EQ(a.sources.clocks().groups().size(), 1);
    ASSERT_EQ(a.sources.clocks().groups().front().subcatches.size(), 128);
    scaledBalance(a, 2);
    scaledBalance(b, 2);
    ASSERT_EQ(a.engine.end(), 0);
    ASSERT_EQ(b.engine.end(), 0);
    EXPECT_NEAR(a.engine.context().mass_balance_2d.error(), 0, 1.e-7);
    EXPECT_NEAR(a.engine.context().mass_balance.routing_error(), 0, 1.e-5);
    sameBinary(name, name + "-resumed", 6);
    sameMesh(name, name + "-resumed", 6);
}
#endif
#endif

namespace {
/**
 * @brief Snapshot sorted evidence filenames for inspection side-effect assertions.
 *
 * Calls root directory setup then iterates without reading contents or removing entries;
 * filesystem/allocation exceptions propagate. Used to compare which files metadata inspection
 * created, not their byte contents.
 * @return Owned set of entry basenames from root, including directories.
 */
std::set<std::string> inspectionFiles() {
    std::set<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(root()))
        names.insert(entry.path().filename().string());
    return names;
}
/**
 * @brief Assert report inspection coverage metadata against expected timing.
 *
 * Nonfatally requires enabled/count/cadence. Zero records checks empty dates and returns; nonzero
 * fatally requires both endpoints then compares dates to addSeconds(start,offset+0.001). Does not
 * derive timing from file contents or modify inspection state.
 *
 * @param[in] report Borrowed report inspection record.
 * @param[in] records Expected report record count; zero expects absent date endpoints.
 * @param[in] cadence Expected reporting interval in seconds.
 * @param[in] start Simulation start as native SWMM calendar days.
 * @param[in] first Expected first report offset in elapsed seconds, before the test 0.001 s date
 *     adjustment.
 * @param[in] last Expected last report offset in elapsed seconds, before the same adjustment.
 */
void inspectionReports(const CheckpointInspection::Reports& report, int records, double cadence,
                       double start, double first, double last) {
    EXPECT_TRUE(report.enabled);
    EXPECT_EQ(report.records, records);
    EXPECT_DOUBLE_EQ(report.cadence_seconds, cadence);
    if (!records) {
        EXPECT_FALSE(report.first_date);
        EXPECT_FALSE(report.last_date);
        return;
    }
    ASSERT_TRUE(report.first_date);
    ASSERT_TRUE(report.last_date);
    EXPECT_DOUBLE_EQ(*report.first_date, datetime::addSeconds(start, first + .001));
    EXPECT_DOUBLE_EQ(*report.last_date, datetime::addSeconds(start, last + .001));
}
}
TEST(EngineCheckpoint, InspectionBinaryCoveragePreservesFreshStateWithDelayedReportStart) {
    for (bool late : {false, true})
        for (double phase : {0., 9.1, 19.1}) {
            const auto name =
                "inspect-binary-" + std::to_string(late) + "-" + std::to_string(phase);
            SCOPED_TRACE(name);
            SWMMEngine a, b;
            ASSERT_NO_THROW(startBinary(a, name, 2, false, late));
            ASSERT_NO_THROW(startBinary(b, name + "-fresh", 2, false, late));
            while (a.context().current_time < phase)
                ASSERT_NO_THROW(step(a));
            const auto cp = root() / (name + ".chk"), before = root() / (name + "-before.chk"),
                       after = root() / (name + "-after.chk");
            ASSERT_EQ(a.saveCheckpoint(cp.string()), "");
            ASSERT_EQ(b.saveCheckpoint(before.string()), "");
            const auto output = bytes(root() / (name + "-fresh.out"));
            const auto files = inspectionFiles();
            CheckpointInspection info;
            ASSERT_EQ(b.inspectCheckpoint(cp.string(), info), "");
            EXPECT_EQ(files, inspectionFiles());
            EXPECT_EQ(bytes(root() / (name + "-fresh.out")), output);
            EXPECT_DOUBLE_EQ(b.context().current_time, 0);
            EXPECT_DOUBLE_EQ(info.engine_seconds, a.context().current_time);
            EXPECT_DOUBLE_EQ(info.engine_date, a.context().current_date);
            EXPECT_DOUBLE_EQ(info.start_date, a.context().options.start_date);
            EXPECT_DOUBLE_EQ(info.report_start_date, a.context().options.report_start);
            EXPECT_EQ(info.checkpoint_bytes, std::filesystem::file_size(cp));
            EXPECT_FALSE(info.completed_source_seconds);
            EXPECT_FALSE(info.driver);
            EXPECT_FALSE(info.mesh.enabled);
            EXPECT_FALSE(info.file_outputs);
            EXPECT_TRUE(info.companions.empty());
            const int records = phase == 0 ? 0 : phase < 10 ? (late ? 0 : 3) : (late ? 2 : 6);
            inspectionReports(info.binary, records, 3, info.start_date, late ? 15 : 3,
                              phase < 10 ? 9 : 18);
            const auto saved = info;
            ASSERT_EQ(b.inspectCheckpoint(cp.string(), info), "");
            EXPECT_EQ(info, saved);
            ASSERT_EQ(b.saveCheckpoint(after.string()), "");
            EXPECT_EQ(bytes(before), bytes(after));
            ASSERT_EQ(b.loadCheckpoint(cp.string()), "");
            compare(a, b, name + "-restored");
            if (phase > 0) {
                EXPECT_FALSE(b.inspectCheckpoint(cp.string(), info).empty());
                EXPECT_EQ(info, saved);
            }
        }
}

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, InspectionSourceAndDueDriverClocksPreserveFreshControllersAcrossProfiles) {
    for (int lid : {0, 1, 2})
        for (int closure : {0, 1, 2}) {
            const auto name =
                "inspect-source-" + std::to_string(lid) + "-" + std::to_string(closure);
            SCOPED_TRACE(name);
            TestWaterDriver da, db;
            Stream recorded, resumed;
            SurfaceSession a, b;
            ASSERT_NO_THROW(startCatchmentProvider(a, name, lid, closure));
            ASSERT_NO_THROW(startCatchmentProvider(b, name + "-fresh", lid, closure));
            ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(da), "");
            ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(db), "");
            const bool detailed = (lid + closure) % 2;
            ASSERT_EQ(
                a.engine.setEngineSourceResultStream(recorded, streamPath(name).string(), detailed),
                "");
            ASSERT_EQ(b.engine.setEngineSourceResultStream(
                          resumed, streamPath(name + "-fresh").string(), detailed),
                      "");
            ASSERT_NO_THROW(drive(a.engine, da, 4.2));
            const auto cp = root() / (name + ".chk"), before = root() / (name + "-before.chk"),
                       after = root() / (name + "-after.chk");
            ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
            ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
            const auto fresh = bytes(resumed.path());
            const auto cursor = resumed.cursor();
            const auto files = inspectionFiles();
            CheckpointInspection info;
            ASSERT_EQ(b.engine.inspectCheckpoint(cp.string(), info), "");
            EXPECT_EQ(files, inspectionFiles());
            EXPECT_DOUBLE_EQ(info.engine_seconds, a.engine.context().current_time);
            ASSERT_TRUE(info.completed_source_seconds);
            EXPECT_DOUBLE_EQ(*info.completed_source_seconds, a.stage.completedEnd());
            EXPECT_LE(*info.completed_source_seconds, info.engine_seconds);
            ASSERT_TRUE(info.driver);
            EXPECT_EQ(info.driver->identity, da.identity());
            EXPECT_EQ(info.driver->next_sequence, da.cursor);
            EXPECT_DOUBLE_EQ(info.driver->next_update_seconds, info.engine_seconds);
            EXPECT_EQ(db.installs, 0);
            EXPECT_EQ(db.cursor, 0);
            EXPECT_EQ(info.source_frames, recorded.cursor().frames);
            EXPECT_EQ(info.source_prefix_bytes, recorded.cursor().bytes);
            EXPECT_EQ(info.detailed_sources, detailed);
            EXPECT_FALSE(info.binary.enabled);
            EXPECT_FALSE(info.mesh.enabled);
            ASSERT_EQ(info.companions.size(), 1);
            EXPECT_EQ(info.companions[0].path, recorded.path());
            EXPECT_EQ(info.companions[0].required_bytes, recorded.cursor().bytes);
            const auto saved = info;
            ASSERT_EQ(b.engine.inspectCheckpoint(cp.string(), info), "");
            EXPECT_EQ(info, saved);
            EXPECT_EQ(resumed.cursor(), cursor);
            EXPECT_EQ(bytes(resumed.path()), fresh);
            ASSERT_EQ(b.engine.saveCheckpoint(after.string()), "");
            EXPECT_EQ(bytes(before), bytes(after));
            ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
            EXPECT_EQ(db.installs, 1);
            equalStreamPrefix(recorded, resumed);
            ASSERT_NO_THROW(drive(a.engine, da, 5.5));
            ASSERT_NO_THROW(drive(b.engine, db, 5.5));
            compare(a.engine, b.engine, name + "-continued");
            equalStreamPrefix(recorded, resumed);
        }
}
TEST(EngineCheckpoint, InspectionExhaustedDriverHasNoFutureUpdateAndDoesNotAdoptItsCursor) {
    TestWaterDriver da, db;
    Stream recorded, resumed;
    SurfaceSession a, b;
    ASSERT_NO_THROW(startCatchmentProvider(a, "inspect-exhausted", 2, 2));
    ASSERT_NO_THROW(startCatchmentProvider(b, "inspect-exhausted-fresh", 2, 2));
    ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(da), "");
    ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(db), "");
    ASSERT_EQ(a.engine.setEngineSourceResultStream(recorded,
                                                   streamPath("inspect-exhausted").string(), true),
              "");
    ASSERT_EQ(b.engine.setEngineSourceResultStream(
                  resumed, streamPath("inspect-exhausted-fresh").string(), true),
              "");
    ASSERT_NO_THROW(drive(a.engine, da, 13.2));
    ASSERT_EQ(da.nextUpdateSeconds(), -1);
    const auto cp = root() / "inspect-exhausted.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    const auto cursor = resumed.cursor();
    const auto source = bytes(resumed.path());
    CheckpointInspection info;
    ASSERT_EQ(b.engine.inspectCheckpoint(cp.string(), info), "");
    ASSERT_TRUE(info.driver);
    EXPECT_EQ(info.driver->next_sequence, 5);
    EXPECT_DOUBLE_EQ(info.driver->next_update_seconds, -1);
    EXPECT_EQ(db.cursor, 0);
    EXPECT_EQ(db.installs, 0);
    EXPECT_EQ(resumed.cursor(), cursor);
    EXPECT_EQ(bytes(resumed.path()), source);
    ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
    EXPECT_EQ(db.cursor, 5);
    EXPECT_EQ(db.installs, 1);
    EXPECT_EQ(db.nextUpdateSeconds(), -1);
    compare(a.engine, b.engine, "inspect-exhausted-restored");
    equalStreamPrefix(recorded, resumed);
}
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
TEST(EngineCheckpoint, InspectionPairedCoverageDistinguishesEmptyDelayedAndDifferentCadences) {
    for (int format : {1, 2, 3, 4})
        for (double phase : {0., 9.1, 19.1}) {
            const auto name =
                "inspect-paired-" + std::to_string(format) + "-" + std::to_string(phase);
            SCOPED_TRACE(name);
            Stream recorded, resumed;
            SurfaceSession a, b;
            ASSERT_NO_THROW(startCatchmentProvider(a, name, 2, 0, true, format));
            ASSERT_NO_THROW(startCatchmentProvider(b, name + "-fresh", 2, 0, true, format));
            ASSERT_EQ(
                a.engine.setEngineSourceResultStream(recorded, streamPath(name).string(), true),
                "");
            ASSERT_EQ(b.engine.setEngineSourceResultStream(
                          resumed, streamPath(name + "-fresh").string(), true),
                      "");
            if (format < 3) {
                ASSERT_EQ(a.engine.enableFileOutputCheckpointing(), "");
                ASSERT_EQ(b.engine.enableFileOutputCheckpointing(), "");
            }
            if (phase > 0)
                ASSERT_NO_THROW(until(a.engine, phase));
            const auto cp = root() / (name + ".chk"), before = root() / (name + "-before.chk"),
                       after = root() / (name + "-after.chk");
            ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
            ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
            const auto binary = bytes(root() / (name + "-fresh.out")),
                       mesh = bytes(root() / (name + "-fresh.h5")), stream = bytes(resumed.path());
            const auto cursor = resumed.cursor();
            const auto files = inspectionFiles();
            CheckpointInspection info;
            ASSERT_EQ(b.engine.inspectCheckpoint(cp.string(), info), "");
            EXPECT_EQ(files, inspectionFiles());
            const bool late = format == 4;
            const int periods = phase == 0 ? 0 : phase < 10 ? (late ? 0 : 3) : (late ? 2 : 6);
            const int mesh_periods = phase == 0 ? 0 : phase < 10 ? (late ? 0 : 2) : (late ? 1 : 3);
            inspectionReports(info.binary, periods, 3, info.start_date, late ? 15 : 3,
                              phase < 10 ? 9 : 18);
            inspectionReports(info.mesh, mesh_periods, 6, info.start_date, late ? 15 : 3,
                              phase < 10 ? 9 : 15);
            EXPECT_DOUBLE_EQ(info.next_report_seconds, a.engine.context().next_report_ms / 1000);
            EXPECT_EQ(info.file_outputs, format < 3);
            EXPECT_FALSE(info.relocatable_bundle);
            EXPECT_FALSE(info.driver);
            EXPECT_EQ(info.companions.size(), format < 3 ? 3 : 1);
            EXPECT_DOUBLE_EQ(b.engine.context().current_time, 0);
            EXPECT_EQ(bytes(root() / (name + "-fresh.out")), binary);
            EXPECT_EQ(bytes(root() / (name + "-fresh.h5")), mesh);
            EXPECT_EQ(resumed.cursor(), cursor);
            EXPECT_EQ(bytes(resumed.path()), stream);
            ASSERT_EQ(b.engine.saveCheckpoint(after.string()), "");
            EXPECT_EQ(physicalCheckpoint(bytes(before)), physicalCheckpoint(bytes(after)));
            ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
            compare(a.engine, b.engine, name + "-restored");
            equalStreamPrefix(recorded, resumed);
        }
}
TEST(EngineCheckpoint, InspectionMovedBundleUsesSavedPrefixAndResolvedCompanionPaths) {
    const std::string name = "inspect-large-moved";
    Stream recorded, resumed;
    SurfaceSession a, b;
    ASSERT_NO_THROW(startScaledSurface(a, name, 12, 32, 128, 2, 0, true, 1));
    ASSERT_NO_THROW(startScaledSurface(b, name + "-fresh", 12, 32, 128, 2, 0, true, 1));
    ASSERT_EQ(a.engine.setEngineSourceResultStream(recorded, streamPath(name).string(), true), "");
    ASSERT_EQ(
        b.engine.setEngineSourceResultStream(resumed, streamPath(name + "-fresh").string(), true),
        "");
    ASSERT_EQ(a.engine.enableFileOutputCheckpointing(), "");
    ASSERT_EQ(b.engine.enableFileOutputCheckpointing(), "");
    ASSERT_NO_THROW(until(a.engine, 2.2));
    const auto cp = root() / (name + ".chk");
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    const auto saved = recorded.cursor();
    const auto time = a.engine.context().current_time, source_time = a.stage.completedEnd();
    ASSERT_NO_THROW(until(a.engine, 6.4));
    EXPECT_GT(recorded.cursor().bytes, saved.bytes);
    const auto folder = root() / (name + " export"), moved = root() / (name + " moved λ");
    ASSERT_EQ(SWMMEngine::packageCheckpointBundle(cp.string(), folder.string()), "");
    std::filesystem::rename(folder, moved);
    const auto binary = bytes(root() / (name + "-fresh.out")),
               mesh = bytes(root() / (name + "-fresh.h5")), fresh = bytes(resumed.path());
    const auto cursor = resumed.cursor();
    const auto files = inspectionFiles();
    CheckpointInspection info;
    ASSERT_EQ(b.engine.inspectCheckpoint(io::path_utf8(moved / "session.chk"), info), "");
    EXPECT_EQ(files, inspectionFiles());
    EXPECT_TRUE(info.relocatable_bundle);
    EXPECT_TRUE(info.file_outputs);
    EXPECT_DOUBLE_EQ(info.engine_seconds, time);
    ASSERT_TRUE(info.completed_source_seconds);
    EXPECT_DOUBLE_EQ(*info.completed_source_seconds, source_time);
    EXPECT_EQ(info.source_frames, saved.frames);
    EXPECT_EQ(info.source_prefix_bytes, saved.bytes);
    ASSERT_EQ(info.companions.size(), 3);
    const char* names[] = {"source.history", "binary.out", "mesh.h5"};
    for (std::size_t i = 0; i < info.companions.size(); ++i) {
        EXPECT_EQ(info.companions[i].path,
                  io::path_utf8(std::filesystem::canonical(moved) / names[i]));
        EXPECT_EQ(info.companions[i].required_bytes, std::filesystem::file_size(moved / names[i]));
    }
    inspectionReports(info.binary, 2, 1, info.start_date, 1, 2);
    inspectionReports(info.mesh, 2, 1, info.start_date, 1, 2);
    EXPECT_EQ(bytes(root() / (name + "-fresh.out")), binary);
    EXPECT_EQ(bytes(root() / (name + "-fresh.h5")), mesh);
    EXPECT_EQ(resumed.cursor(), cursor);
    EXPECT_EQ(bytes(resumed.path()), fresh);
    ASSERT_EQ(b.engine.loadCheckpoint(io::path_utf8(moved / "session.chk")), "");
    ASSERT_NO_THROW(until(b.engine, 6.4));
    compare(a.engine, b.engine, name + "-continued");
    equalStreamPrefix(recorded, resumed);
}
TEST(EngineCheckpoint, InspectionRejectionPreservesMetadataDriverAndAllFreshOutputsWithRetry) {
    const std::string name = "inspect-rejection";
    TestWaterDriver da;
    Stream recorded;
    SurfaceSession a;
    ASSERT_NO_THROW(startCatchmentProvider(a, name, 2, 2, true, 2));
    ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(da), "");
    ASSERT_EQ(a.engine.setEngineSourceResultStream(recorded, streamPath(name).string(), true), "");
    ASSERT_EQ(a.engine.enableFileOutputCheckpointing(), "");
    ASSERT_NO_THROW(drive(a.engine, da, 4.2));
    const auto cp = root() / (name + ".chk");
    ASSERT_EQ(a.engine.saveCheckpoint(cp.string()), "");
    for (int variant = 0; variant < 5; ++variant) {
        SCOPED_TRACE(variant);
        const auto target = name + "-" + std::to_string(variant);
        TestWaterDriver db;
        Stream resumed;
        SurfaceSession b;
        ASSERT_NO_THROW(startCatchmentProvider(b, target, 2, 2, true, 2));
        ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(db), "");
        ASSERT_EQ(b.engine.setEngineSourceResultStream(resumed, streamPath(target).string(), true),
                  "");
        ASSERT_EQ(b.engine.enableFileOutputCheckpointing(), "");
        const auto before = root() / (target + "-before.chk"),
                   after = root() / (target + "-after.chk");
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()), "");
        const auto binary = bytes(root() / (target + ".out")),
                   mesh = bytes(root() / (target + ".h5")), fresh = bytes(resumed.path());
        const auto cursor = resumed.cursor();
        auto input = cp;
        std::filesystem::path damaged;
        std::string original;
        if (variant == 0) {
            input = root() / (target + "-bad.chk");
            auto bad = bytes(cp);
            bad.back() ^= 1;
            write(input, bad);
        }
        if (variant == 1)
            db.reject = true;
        if (variant == 2)
            b.engine.context().subcatches.width[0] += 1;
        if (variant == 3) {
            damaged = recorded.path();
            original = bytes(damaged);
            auto bad = original;
            bad.back() ^= 1;
            write(damaged, bad);
        }
        if (variant == 4) {
            FileBundle bundle(cp);
            damaged = bundle.meshFile().path;
            original = bytes(damaged);
            const auto file = H5Fopen(damaged.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
            ASSERT_GE(file, 0);
            const auto time = H5Dopen2(file, "time", H5P_DEFAULT);
            ASSERT_GE(time, 0);
            double date;
            ASSERT_GE(H5Dread(time, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, &date), 0);
            date += 1;
            ASSERT_GE(H5Dwrite(time, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, &date), 0);
            H5Dclose(time);
            H5Fclose(file);
            bundle.rehash(false);
            input = root() / (target + "-schema.chk");
            write(input, bundle.encode());
        }
        CheckpointInspection info;
        info.schema_version = 99;
        info.engine_seconds = 123;
        const auto sentinel = info;
        const auto files = inspectionFiles();
        EXPECT_FALSE(b.engine.inspectCheckpoint(input.string(), info).empty());
        EXPECT_EQ(info, sentinel);
        EXPECT_EQ(files, inspectionFiles());
        EXPECT_DOUBLE_EQ(b.engine.context().current_time, 0);
        EXPECT_EQ(db.installs, 0);
        EXPECT_EQ(db.cursor, 0);
        EXPECT_EQ(bytes(root() / (target + ".out")), binary);
        EXPECT_EQ(bytes(root() / (target + ".h5")), mesh);
        EXPECT_EQ(resumed.cursor(), cursor);
        EXPECT_EQ(bytes(resumed.path()), fresh);
        db.reject = false;
        if (variant == 2)
            b.engine.context().subcatches.width[0] -= 1;
        if (!damaged.empty())
            write(damaged, original);
        ASSERT_EQ(b.engine.saveCheckpoint(after.string()), "");
        EXPECT_EQ(physicalCheckpoint(bytes(before)), physicalCheckpoint(bytes(after)));
        ASSERT_EQ(b.engine.inspectCheckpoint(cp.string(), info), "");
        ASSERT_TRUE(info.driver);
        EXPECT_EQ(db.installs, 0);
        ASSERT_EQ(b.engine.loadCheckpoint(cp.string()), "");
        EXPECT_EQ(db.installs, 1);
        compare(a.engine, b.engine, target + "-restored");
        equalStreamPrefix(recorded, resumed);
    }
}
#endif
#endif

namespace {
struct PublicInspection {
    SWMM_CheckpointInspection handle = nullptr;
    /**
     * @brief Destroy the owned public checkpoint inspection snapshot.
     *
     * Passes the stored handle, including nullptr, to inspection_destroy. Any borrowed inspection data
     * expires; does not affect the engine/checkpoint artifact that produced the independent snapshot.
     */
    ~PublicInspection() { swmm_checkpoint_inspection_destroy(handle); }
    /**
     * @brief Copy owned inspection metadata JSON through public size/fill queries.
     *
     * Requires a valid inspection handle whose size query returns at least one byte. Throws
     * runtime_error on either rejected query or missing trailing NUL. Uses exact first-reported size
     * without a retry; inspection snapshots are immutable, so sizing/fill refer to stable data.
     * @return Owned NUL-stripped JSON text, independent of the inspection handle lifetime.
     */
    std::string json() const {
        size_t size = 0;
        if (swmm_checkpoint_inspection_json(handle, nullptr, 0, &size) != SWMM_OK)
            throw std::runtime_error(swmm_checkpoint_last_error_msg());
        std::string buffer(size, '!');
        if (swmm_checkpoint_inspection_json(handle, buffer.data(), buffer.size(), &size) != SWMM_OK)
            throw std::runtime_error(swmm_checkpoint_last_error_msg());
        if (buffer.back() != 0)
            throw std::runtime_error("Inspection JSON is not terminated.");
        buffer.pop_back();
        return buffer;
    }
};
}
TEST(EngineCheckpoint, PublicMetadataJsonUsesClassicLocaleAndRoundTripPrecision) {
    /** @cond */
    struct Comma : std::numpunct<char> {
        /**
         * @brief Inject a decimal comma into the test numeric locale.
         *
         * Local numpunct override used to verify JSON formatting remains locale-independent.
         * @return Comma character unconditionally.
         */
        char do_decimal_point() const override { return ','; }
        /**
         * @brief Inject a period thousands separator into the test numeric locale.
         *
         * Does not format JSON itself; the test installs this facet in the global locale.
         * @return Period character unconditionally.
         */
        char do_thousands_sep() const override { return '.'; }
        /**
         * @brief Request three-digit grouping in the temporary test locale.
         *
         * Used only by the local numpunct test facet; no global locale mutation occurs in this override.
         * @return Owned one-byte grouping pattern with value three.
         */
        std::string do_grouping() const override { return "\3"; }
    };
    /** @endcond */
    /** @cond */
    struct Restore {
        std::locale previous;
        /**
         * @brief Restore the global locale captured before the inspection JSON test.
         *
         * Invokes locale::global(previous) at scope exit, including normal assertion unwinding. Caller test
         * must serialize process-global locale changes; this guard does not catch restoration exceptions.
         */
        ~Restore() { std::locale::global(previous); }
    } restore{std::locale()};
    /** @endcond */
    std::locale::global(std::locale(std::locale::classic(), new Comma));
    CheckpointInspection info;
    info.engine_seconds = std::nextafter(1., 2.);
    info.engine_date = 12345.125;
    const auto json = info.json();
    EXPECT_NE(json.find("\"engine_seconds\":1.0000000000000002"), std::string::npos);
    EXPECT_NE(json.find("\"engine_date\":12345.125"), std::string::npos);
    EXPECT_EQ(json.find("12.345,125"), std::string::npos);
}
TEST(EngineCheckpoint, PublicMetadataJsonPreservesLargeCountsUnicodeAndRejectsInvalidFields) {
    CheckpointInspection info;
    info.engine_seconds = 12.125;
    info.engine_date = 36526.000000011576;
    info.binary.enabled = true;
    info.binary.records = UINT64_MAX;
    info.binary.first_date = info.engine_date;
    info.binary.last_date = info.engine_date;
    info.source_frames = 9007199254740993ULL;
    info.checkpoint_bytes = UINT64_MAX;
    info.driver = CheckpointInspection::Driver{std::string("révision \"雪\"\n\\\t") + char(0),
                                               UINT64_MAX, -1};
    info.companions.push_back({CheckpointInspection::Companion::Role::Source,
                               "folder/" + std::string(5000, 'x') + "/café 雪.history",
                               UINT64_MAX});
    const auto json = info.json();
    write(root() / "public-metadata-schema.json", json);
    EXPECT_NE(json.find("\"records\":\"18446744073709551615\""), std::string::npos);
    EXPECT_NE(json.find("\"source_frames\":\"9007199254740993\""), std::string::npos);
    EXPECT_NE(json.find("\"next_update_seconds\":-1"), std::string::npos);
    EXPECT_NE(json.find("\"completed_source_seconds\":null"), std::string::npos);
    EXPECT_NE(json.find("\\u000a\\\\\\u0009\\u0000"), std::string::npos);
    EXPECT_NE(json.find("café 雪.history"), std::string::npos);
    EXPECT_NE(json.find("\"engine_seconds\":12.125"), std::string::npos);
    info.schema_version = 2;
    EXPECT_THROW(info.json(), std::logic_error);
    info.schema_version = 1;
    info.engine_date = std::numeric_limits<double>::infinity();
    EXPECT_THROW(info.json(), std::logic_error);
    info.engine_date = 1;
    info.driver->identity = std::string("\xed\xa0\x80", 3);
    EXPECT_THROW(info.json(), std::logic_error);
    for (const std::string invalid :
         {std::string("\xc0\x80", 2), std::string("\xf4\x90\x80\x80", 4),
          std::string("\xe2\x82", 2), std::string("\x80", 1)})
        EXPECT_FALSE(detail::checkpointUtf8(invalid));
    EXPECT_TRUE(detail::checkpointUtf8("café 雪 🌊"));
}
TEST(EngineCheckpoint, PublicCapabilitiesAndLifecycleDoNotAttachOrMutateOrdinarySessions) {
    SWMMEngine e;
    unsigned long flags = 123, ready = 123;
    EXPECT_EQ(swmm_checkpoint_capabilities(nullptr, &flags), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(flags, 123);
    EXPECT_EQ(swmm_checkpoint_capabilities(&e, nullptr), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_checkpoint_capabilities(&e, &flags), SWMM_OK);
    EXPECT_TRUE(flags & SWMM_CHECKPOINT_INSPECTION);
    EXPECT_TRUE(flags & SWMM_CHECKPOINT_RESTORE);
    EXPECT_TRUE(flags & SWMM_CHECKPOINT_BINARY_PROFILE);
    EXPECT_EQ(bool(flags & SWMM_CHECKPOINT_SAVE_NEW), io::checkpointNoReplaceAvailable());
#ifdef OPENSWMM_HAS_2D
    EXPECT_TRUE(flags & SWMM_CHECKPOINT_SURFACE_PROFILE);
    EXPECT_TRUE(flags & SWMM_CHECKPOINT_SOURCE_STREAM);
#else
    EXPECT_FALSE(flags & SWMM_CHECKPOINT_SURFACE_PROFILE);
    EXPECT_FALSE(flags & SWMM_CHECKPOINT_SOURCE_STREAM);
#endif
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
    EXPECT_TRUE(flags & SWMM_CHECKPOINT_FILE_PROFILE);
    EXPECT_EQ(bool(flags & SWMM_CHECKPOINT_BUNDLE_EXPORT), io::checkpointNoReplaceAvailable());
#else
    EXPECT_FALSE(flags & SWMM_CHECKPOINT_FILE_PROFILE);
    EXPECT_FALSE(flags & SWMM_CHECKPOINT_BUNDLE_EXPORT);
    EXPECT_EQ(swmm_checkpoint_export_bundle("a.chk", "new-folder"), SWMM_ERR_LIFECYCLE);
#endif
    ASSERT_EQ(swmm_checkpoint_session_status(&e, &ready), SWMM_OK);
    EXPECT_EQ(ready, 0);
    PublicInspection saved;
    EXPECT_EQ(swmm_checkpoint_inspect(&e, "a.chk", &saved.handle), SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(saved.handle, nullptr);
    EXPECT_EQ(swmm_checkpoint_save(&e, "a.chk"), SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(swmm_checkpoint_restore(&e, "a.chk"), SWMM_ERR_LIFECYCLE);
    ASSERT_NO_THROW(start(e, "public-unattached", 0, 0, false, false));
    ASSERT_EQ(swmm_checkpoint_session_status(&e, &ready), SWMM_OK);
    EXPECT_EQ(ready, 0);
    EXPECT_EQ(swmm_checkpoint_save(&e, "a.chk"), SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(e.context().error_code, 0);
    ASSERT_EQ(e.enableCheckpointing(), "");
    ASSERT_EQ(swmm_checkpoint_session_status(&e, &ready), SWMM_OK);
    EXPECT_EQ(ready, SWMM_CHECKPOINT_SESSION_ATTACHED | SWMM_CHECKPOINT_SESSION_BOUNDARY |
                         SWMM_CHECKPOINT_SESSION_FRESH);
    ASSERT_NO_THROW(step(e));
    ASSERT_EQ(swmm_checkpoint_session_status(&e, &ready), SWMM_OK);
    EXPECT_EQ(ready, SWMM_CHECKPOINT_SESSION_ATTACHED | SWMM_CHECKPOINT_SESSION_BOUNDARY);
    EXPECT_EQ(swmm_checkpoint_inspect(&e, "a.chk", &saved.handle), SWMM_ERR_LIFECYCLE);
    e.close();
    ASSERT_EQ(swmm_checkpoint_session_status(&e, &ready), SWMM_OK);
    EXPECT_FALSE(ready & SWMM_CHECKPOINT_SESSION_BOUNDARY);
}
TEST(EngineCheckpoint, PublicInspectionOwnsSnapshotAndPreservesFreshBinaryStateAcrossFailures) {
    for (bool late : {false, true}) {
        const auto name = late ? "public-binary-late" : "public-binary";
        SWMMEngine a;
        auto b = std::make_unique<SWMMEngine>();
        ASSERT_NO_THROW(startBinary(a, name, 2, false, late));
        ASSERT_NO_THROW(startBinary(*b, std::string(name) + "-fresh", 2, false, late));
        while (a.context().current_time < 8)
            ASSERT_NO_THROW(step(a));
        const auto cp = root() / (std::string(name) + " café 雪.chk"),
                   before = root() / (std::string(name) + "-before.chk"),
                   after = root() / (std::string(name) + "-after.chk");
        const auto cp_name = io::path_utf8(cp);
        ASSERT_EQ(swmm_checkpoint_save(&a, cp_name.c_str()), SWMM_OK);
        ASSERT_EQ(b->saveCheckpoint(before.string()), "");
        const auto fresh = bytes(root() / (std::string(name) + "-fresh.out"));
        CheckpointInspection native;
        ASSERT_EQ(b->inspectCheckpoint(cp_name, native), "");
        PublicInspection snapshot;
        ASSERT_EQ(swmm_checkpoint_inspect(b.get(), cp_name.c_str(), &snapshot.handle), SWMM_OK);
        const auto json = snapshot.json();
        EXPECT_EQ(json, native.json());
        write(root() / (std::string(name) + ".json"), json);
        EXPECT_EQ(b->context().current_time, 0);
        EXPECT_EQ(bytes(root() / (std::string(name) + "-fresh.out")), fresh);
        size_t required = 123;
        char buffer[17];
        std::fill(std::begin(buffer), std::end(buffer), '!');
        EXPECT_EQ(
            swmm_checkpoint_inspection_json(snapshot.handle, buffer, sizeof buffer, &required),
            SWMM_ERR_BADPARAM);
        EXPECT_EQ(required, json.size() + 1);
        EXPECT_EQ(std::string(buffer, sizeof buffer), std::string(sizeof buffer, '!'));
        required = 123;
        EXPECT_EQ(swmm_checkpoint_inspection_json(snapshot.handle, nullptr, 1, &required),
                  SWMM_ERR_BADPARAM);
        EXPECT_EQ(required, 123);
        EXPECT_EQ(swmm_checkpoint_inspection_json(snapshot.handle, buffer, sizeof buffer, nullptr),
                  SWMM_ERR_BADPARAM);
        EXPECT_EQ(swmm_checkpoint_inspection_json(nullptr, buffer, sizeof buffer, &required),
                  SWMM_ERR_BADHANDLE);
        EXPECT_EQ(required, 123);
        const auto original = snapshot.handle;
        const auto bad = root() / (std::string(name) + "-bad.chk");
        auto corrupted = bytes(cp);
        corrupted.back() ^= 1;
        write(bad, corrupted);
        EXPECT_EQ(swmm_checkpoint_inspect(b.get(), bad.string().c_str(), &snapshot.handle),
                  SWMM_ERR_IO);
        EXPECT_EQ(snapshot.handle, original);
        const std::string diagnostic = swmm_checkpoint_last_error_msg();
        EXPECT_FALSE(diagnostic.empty());
        EXPECT_EQ(b->context().error_code, 0);
        EXPECT_EQ(swmm_checkpoint_inspect(b.get(), nullptr, &snapshot.handle), SWMM_ERR_BADPARAM);
        EXPECT_EQ(snapshot.handle, original);
        EXPECT_EQ(swmm_checkpoint_inspect(b.get(), "", &snapshot.handle), SWMM_ERR_BADPARAM);
        EXPECT_EQ(swmm_checkpoint_inspect(b.get(), "\xc0\x80", &snapshot.handle),
                  SWMM_ERR_BADPARAM);
        EXPECT_EQ(swmm_checkpoint_inspect(b.get(), cp_name.c_str(), nullptr), SWMM_ERR_BADPARAM);
        ASSERT_EQ(b->saveCheckpoint(after.string()), "");
        EXPECT_EQ(bytes(before), bytes(after));
        ASSERT_EQ(swmm_checkpoint_restore(b.get(), cp_name.c_str()), SWMM_OK);
        compare(a, *b, std::string(name) + "-restored");
        EXPECT_EQ(swmm_checkpoint_inspect(b.get(), cp_name.c_str(), &snapshot.handle),
                  SWMM_ERR_LIFECYCLE);
        EXPECT_EQ(snapshot.handle, original);
        b.reset();
        EXPECT_EQ(snapshot.json(), json);
        EXPECT_EQ(snapshot.json(), json);
    }
    swmm_checkpoint_inspection_destroy(nullptr);
}
TEST(EngineCheckpoint, PublicSaveRefusesFilesDirectoriesAndSymlinksButInternalReplaceRemains) {
    SWMMEngine a;
    ASSERT_NO_THROW(start(a, "public-new-save", 2));
    const auto cp = root() / "public-new-save.chk";
    ASSERT_EQ(swmm_checkpoint_save(&a, cp.string().c_str()), SWMM_OK);
    const auto original = bytes(cp);
    ASSERT_NO_THROW(step(a));
    EXPECT_EQ(swmm_checkpoint_save(&a, cp.string().c_str()), SWMM_ERR_IO);
    EXPECT_EQ(bytes(cp), original);
    ASSERT_EQ(a.saveCheckpoint(cp.string()), "");
    EXPECT_NE(bytes(cp), original);
    const auto unrelated = root() / "public-unrelated.chk";
    write(unrelated, "untouched");
    EXPECT_EQ(swmm_checkpoint_save(&a, unrelated.string().c_str()), SWMM_ERR_IO);
    EXPECT_EQ(bytes(unrelated), "untouched");
    const auto dir = root() / "public-save-directory";
    std::filesystem::create_directory(dir);
    EXPECT_EQ(swmm_checkpoint_save(&a, dir.string().c_str()), SWMM_ERR_IO);
    EXPECT_TRUE(std::filesystem::is_directory(dir));
    const auto link = root() / "public-save-symlink";
    std::filesystem::create_symlink(root() / "public-absent-target", link);
    EXPECT_EQ(swmm_checkpoint_save(&a, link.string().c_str()), SWMM_ERR_IO);
    EXPECT_TRUE(std::filesystem::is_symlink(link));
    EXPECT_FALSE(std::filesystem::exists(root() / "public-absent-target"));
    EXPECT_EQ(swmm_checkpoint_save(&a, ""), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_checkpoint_save(&a, "\xf4\x90\x80\x80"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(a.context().error_code, 0);
}
TEST(EngineCheckpoint, PublicSaveConcurrentWritersPublishOneCompleteCheckpoint) {
    SWMMEngine a, b;
    ASSERT_NO_THROW(start(a, "public-race-a", 1));
    ASSERT_NO_THROW(start(b, "public-race-b", 1));
    ASSERT_NO_THROW(step(a));
    ASSERT_NO_THROW(step(b));
    ASSERT_NO_THROW(step(b));
    const auto expected_a = root() / "public-race-a.chk", expected_b = root() / "public-race-b.chk",
               destination = root() / "public-race.chk";
    ASSERT_EQ(a.saveCheckpoint(expected_a.string()), "");
    ASSERT_EQ(b.saveCheckpoint(expected_b.string()), "");
    std::promise<void> begin;
    auto trigger = begin.get_future().share();
    auto save = [&](SWMMEngine& e) {
        trigger.wait();
        const auto rc = swmm_checkpoint_save(&e, destination.string().c_str());
        return std::pair{rc, std::string(swmm_checkpoint_last_error_msg())};
    };
    auto left = std::async(std::launch::async, [&] { return save(a); });
    auto right = std::async(std::launch::async, [&] { return save(b); });
    begin.set_value();
    const auto l = left.get(), r = right.get();
    EXPECT_NE(l.first == SWMM_OK, r.first == SWMM_OK);
    EXPECT_EQ(l.first == SWMM_OK ? r.first : l.first, SWMM_ERR_IO);
    EXPECT_FALSE(l.first == SWMM_OK ? r.second.empty() : l.second.empty());
    EXPECT_EQ(bytes(destination), bytes(l.first == SWMM_OK ? expected_a : expected_b));
    for (const auto& p : std::filesystem::directory_iterator(root()))
        EXPECT_FALSE(p.path().filename().string().starts_with("public-race.chk.session-part-"));
}
TEST(EngineCheckpoint, PublicNoReplaceRefusesDestinationCreatedAfterPrivatePreparation) {
    const auto staging = root() / "public-no-replace-private",
               destination = root() / "public-no-replace-target";
    write(staging, "complete prepared state");
    write(destination, "concurrent owner");
    EXPECT_THROW(io::publishCheckpointNoReplace(staging, destination), std::runtime_error);
    EXPECT_EQ(bytes(destination), "concurrent owner");
    EXPECT_EQ(bytes(staging), "complete prepared state");
    std::filesystem::remove(destination);
    ASSERT_NO_THROW(io::publishCheckpointNoReplace(staging, destination));
    EXPECT_EQ(bytes(destination), "complete prepared state");
    EXPECT_FALSE(std::filesystem::exists(staging));
}
TEST(EngineCheckpoint, PublicDiagnosticsRemainIndependentAcrossThreadsAndEngineErrors) {
    SWMMEngine e;
    ASSERT_NO_THROW(start(e, "public-diagnostic"));
    EXPECT_EQ(swmm_checkpoint_save(nullptr, "x.chk"), SWMM_ERR_BADHANDLE);
    const std::string message = swmm_checkpoint_last_error_msg();
    auto other = std::async(std::launch::async, [] {
        size_t required = 17;
        const auto rc = swmm_checkpoint_inspection_json(nullptr, nullptr, 0, &required);
        return std::pair{rc, std::string(swmm_checkpoint_last_error_msg())};
    });
    const auto child = other.get();
    EXPECT_EQ(child.first, SWMM_ERR_BADHANDLE);
    EXPECT_NE(child.second, message);
    EXPECT_EQ(swmm_checkpoint_last_error_msg(), message);
    EXPECT_EQ(e.context().error_code, 0);
    EXPECT_TRUE(e.context().error_message.empty());
    unsigned long supported = 0;
    ASSERT_EQ(swmm_checkpoint_capabilities(&e, &supported), SWMM_OK);
    EXPECT_STREQ(swmm_checkpoint_last_error_msg(), "");
    EXPECT_EQ(swmm_checkpoint_restore(&e, ""), SWMM_ERR_BADPARAM);
    const std::string last = swmm_checkpoint_last_error_msg();
    e.close();
    EXPECT_EQ(swmm_checkpoint_last_error_msg(), last);
}
#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, PublicInspectionPreservesSourceDueDriversAcrossLidAndAquiferProfiles) {
    for (int lid : {0, 1, 2})
        for (int closure : {0, 1, 2}) {
            const auto name =
                "public-source-" + std::to_string(lid) + "-" + std::to_string(closure);
            TestWaterDriver da, db;
            Stream recorded, resumed;
            SurfaceSession a, b;
            ASSERT_NO_THROW(startCatchmentProvider(a, name, lid, closure));
            ASSERT_NO_THROW(startCatchmentProvider(b, name + "-fresh", lid, closure));
            ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(da), "");
            ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(db), "");
            ASSERT_EQ(
                a.engine.setEngineSourceResultStream(recorded, streamPath(name).string(), true),
                "");
            ASSERT_EQ(b.engine.setEngineSourceResultStream(
                          resumed, streamPath(name + "-fresh").string(), true),
                      "");
            ASSERT_NO_THROW(drive(a.engine, da, 4.2));
            const auto cp = root() / (name + ".chk");
            ASSERT_EQ(swmm_checkpoint_save(&a.engine, cp.string().c_str()), SWMM_OK);
            const auto cursor = resumed.cursor();
            const auto fresh = bytes(resumed.path());
            const auto files = inspectionFiles();
            CheckpointInspection native;
            ASSERT_EQ(b.engine.inspectCheckpoint(cp.string(), native), "");
            PublicInspection snapshot;
            ASSERT_EQ(swmm_checkpoint_inspect(&b.engine, cp.string().c_str(), &snapshot.handle),
                      SWMM_OK);
            EXPECT_EQ(snapshot.json(), native.json());
            ASSERT_TRUE(native.driver);
            EXPECT_EQ(native.driver->next_update_seconds, native.engine_seconds);
            EXPECT_EQ(db.installs, 0);
            EXPECT_EQ(db.cursor, 0);
            EXPECT_EQ(resumed.cursor(), cursor);
            EXPECT_EQ(bytes(resumed.path()), fresh);
            EXPECT_EQ(files, inspectionFiles());
            write(root() / (name + ".json"), snapshot.json());
            ASSERT_EQ(swmm_checkpoint_restore(&b.engine, cp.string().c_str()), SWMM_OK);
            EXPECT_EQ(db.installs, 1);
            equalStreamPrefix(recorded, resumed);
            ASSERT_NO_THROW(drive(a.engine, da, 5.5));
            ASSERT_NO_THROW(drive(b.engine, db, 5.5));
            compare(a.engine, b.engine, name + "-continued");
            equalStreamPrefix(recorded, resumed);
        }
}
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
TEST(EngineCheckpoint, PublicFileSaveAndBundleExportKeepSavedPrefixAndPairedCoverage) {
    TestWaterDriver da, db;
    Stream recorded, resumed;
    SurfaceSession a, b;
    ASSERT_NO_THROW(startCatchmentProvider(a, "public-file", 2, 1, true, 2));
    ASSERT_EQ(a.engine.attachRuntimeWaterDriverCheckpoint(da), "");
    ASSERT_EQ(
        a.engine.setEngineSourceResultStream(recorded, streamPath("public-file").string(), true),
        "");
    ASSERT_EQ(a.engine.enableFileOutputCheckpointing(), "");
    ASSERT_NO_THROW(drive(a.engine, da, 7.75));
    const auto cp = root() / "public-file.chk";
    ASSERT_EQ(swmm_checkpoint_save(&a.engine, cp.string().c_str()), SWMM_OK);
    const auto saved = bytes(cp);
    FileBundle envelope(cp);
    const auto source = recorded.cursor();
    const auto binary = bytes(envelope.binaryFile().path), mesh = bytes(envelope.meshFile().path);
    const auto files = inspectionFiles();
    ASSERT_NO_THROW(drive(a.engine, da, 13.2));
    EXPECT_EQ(swmm_checkpoint_save(&a.engine, cp.string().c_str()), SWMM_ERR_IO);
    EXPECT_EQ(bytes(cp), saved);
    EXPECT_EQ(bytes(envelope.binaryFile().path), binary);
    EXPECT_EQ(bytes(envelope.meshFile().path), mesh);
    EXPECT_EQ(inspectionFiles(), files);
    const auto folder = root() / "public export café 雪";
    ASSERT_EQ(swmm_checkpoint_export_bundle(cp.string().c_str(), io::path_utf8(folder).c_str()),
              SWMM_OK);
    EXPECT_EQ(std::filesystem::file_size(folder / "source.history"), source.bytes);
    EXPECT_EQ(bytes(folder / "binary.out"), binary);
    EXPECT_EQ(bytes(folder / "mesh.h5"), mesh);
    EXPECT_EQ(swmm_checkpoint_export_bundle(cp.string().c_str(), io::path_utf8(folder).c_str()),
              SWMM_ERR_IO);
    EXPECT_EQ(bytes(folder / "binary.out"), binary);
    const auto moved = root() / "public moved café 雪";
    std::filesystem::rename(folder, moved);
    const auto bundled = moved / "session.chk";
    ASSERT_NO_THROW(startCatchmentProvider(b, "public-file-fresh", 2, 1, true, 2));
    ASSERT_EQ(b.engine.attachRuntimeWaterDriverCheckpoint(db), "");
    ASSERT_EQ(b.engine.setEngineSourceResultStream(resumed,
                                                   streamPath("public-file-fresh").string(), true),
              "");
    ASSERT_EQ(b.engine.enableFileOutputCheckpointing(), "");
    CheckpointInspection native;
    ASSERT_EQ(b.engine.inspectCheckpoint(io::path_utf8(bundled), native), "");
    PublicInspection snapshot;
    ASSERT_EQ(swmm_checkpoint_inspect(&b.engine, io::path_utf8(bundled).c_str(), &snapshot.handle),
              SWMM_OK);
    EXPECT_EQ(snapshot.json(), native.json());
    write(root() / "public-file.json", snapshot.json());
    EXPECT_TRUE(native.relocatable_bundle);
    EXPECT_EQ(native.source_prefix_bytes, source.bytes);
    EXPECT_TRUE(native.binary.enabled);
    EXPECT_TRUE(native.mesh.enabled);
    EXPECT_EQ(db.installs, 0);
    ASSERT_EQ(swmm_checkpoint_restore(&b.engine, io::path_utf8(bundled).c_str()), SWMM_OK);
    EXPECT_EQ(db.installs, 1);
    ASSERT_NO_THROW(drive(b.engine, db, 13.2));
    compare(a.engine, b.engine, "public-file-continued");
    equalStreamPrefix(recorded, resumed);
}
#endif
#endif

namespace {
/**
 * @brief Build version-one public managed-session options without owning optional strings.
 *
 * Does not allocate/copy pointed strings, validate supported output modes or start a session.
 *
 * @param[in] output Public checkpoint output mode selector copied without validation.
 * @param[in] stream Optional borrowed stream path pointer; must remain valid until options are
 *     consumed by the session call.
 * @param[in] token Optional borrowed ownership token pointer with the same consumption lifetime.
 * @param[in] detailed Select detailed source reporting with integer flag one when true.
 * @return Options value with current struct size/version, raw pointers and requested selectors.
 */
SWMM_CheckpointSessionOptions managedOptions(int output = 0, const char* stream = nullptr,
                                             const char* token = nullptr, bool detailed = false) {
    return {sizeof(SWMM_CheckpointSessionOptions), 1, output, stream, detailed ? 1 : 0, token};
}
/**
 * @brief Author the baseline Richards 1D deck and open a fresh managed-session candidate.
 *
 * Uses temporary start fixture with LID mode 2 and controls, closes it, then opens the generated
 * deck on e. Temporary close status is ignored. Open rejection throws context diagnostic, leaving
 * partial e for caller cleanup; managed session start is a separate operation.
 *
 * @param[in,out] e Caller-owned openable engine, left opened but uninitialized on success.
 * @param[in] name Raw evidence stem below root for -model input, report and optional output.
 * @param[in] output Nonzero requests a binary output destination at open; zero passes nullptr.
 */
void managed1dOpen(SWMMEngine& e, const std::string& name, int output = 0) {
    SWMMEngine authored;
    start(authored, name + "-model", 2, 0, true, false);
    authored.close();
    const auto inp = root() / (name + "-model.inp"), rpt = root() / (name + ".rpt"),
               out = root() / (name + ".out");
    if (e.open(inp.string().c_str(), rpt.string().c_str(), output ? out.string().c_str() : nullptr))
        throw std::runtime_error(e.context().error_message);
}
}
TEST(EngineCheckpoint, ManagedSessionOneDimensionalProfilesOwnLifetimeAndContinueAfterRestore) {
    for (int mode : {0, 1}) {
        SWMMEngine a, b;
        const auto name = "managed-1d-" + std::to_string(mode);
        ASSERT_NO_THROW(managed1dOpen(a, name + "-a", mode));
        ASSERT_NO_THROW(managed1dOpen(b, name + "-b", mode));
        const auto options = managedOptions(mode);
        ASSERT_EQ(swmm_checkpoint_start_session(&a, &options), SWMM_OK)
            << swmm_checkpoint_last_error_msg();
        ASSERT_EQ(swmm_checkpoint_start_session(&b, &options), SWMM_OK)
            << swmm_checkpoint_last_error_msg();
        unsigned long status = 0;
        ASSERT_EQ(swmm_checkpoint_session_status(&a, &status), SWMM_OK);
        EXPECT_TRUE(status & SWMM_CHECKPOINT_SESSION_MANAGED);
        EXPECT_EQ(swmm_checkpoint_start_session(&a, &options), SWMM_ERR_LIFECYCLE);
        for (int k = 0; k < 9; ++k)
            ASSERT_NO_THROW(step(a));
        const auto cp = root() / (name + ".chk");
        ASSERT_EQ(swmm_checkpoint_save(&a, cp.string().c_str()), SWMM_OK);
        PublicInspection info;
        ASSERT_EQ(swmm_checkpoint_inspect(&b, cp.string().c_str(), &info.handle), SWMM_OK);
        ASSERT_EQ(swmm_checkpoint_restore(&b, cp.string().c_str()), SWMM_OK);
        compare(a, b, name + "-restored");
        for (int k = 0; k < 5; ++k) {
            step(a);
            step(b);
        }
        compare(a, b, name + "-continued");
        const auto json = info.json();
        a.close();
        b.close();
        EXPECT_EQ(info.json(), json);
        ASSERT_EQ(swmm_checkpoint_session_status(&b, &status), SWMM_OK);
        EXPECT_FALSE(status & SWMM_CHECKPOINT_SESSION_MANAGED);
    }
}
TEST(EngineCheckpoint,
     ManagedSessionPreflightRefusesSchemaExistingAndAliasedDestinationsWithoutInitialization) {
    SWMMEngine e;
    ASSERT_NO_THROW(managed1dOpen(e, "managed-preflight", 1));
    auto options = managedOptions(1);
    options.schema_version = 2;
    EXPECT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_ERR_BADPARAM);
    options.schema_version = 1;
    options.struct_size = 0;
    EXPECT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_ERR_BADPARAM);
    options.struct_size = sizeof options;
    options.detailed_source_intervals = 2;
    EXPECT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_ERR_BADPARAM);
    options.detailed_source_intervals = 0;
    write(root() / "managed-preflight.out", "protected");
    EXPECT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_ERR_IO);
    EXPECT_EQ(bytes(root() / "managed-preflight.out"), "protected");
    EXPECT_EQ(e.context().state, EngineState::OPENED);
    std::filesystem::remove(root() / "managed-preflight.out");
    EXPECT_EQ(swmm_checkpoint_start_session(nullptr, &options), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_checkpoint_start_session(&e, nullptr), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_OK)
        << swmm_checkpoint_last_error_msg();
    e.close();
    SWMMEngine alias;
    const auto model = root() / "managed-preflight-model.inp", path = root() / "managed-alias.out";
    ASSERT_EQ(alias.open(model.string().c_str(), path.string().c_str(), path.string().c_str()), 0);
    EXPECT_EQ(swmm_checkpoint_start_session(&alias, &options), SWMM_ERR_IO);
    EXPECT_EQ(alias.context().state, EngineState::OPENED);
    EXPECT_FALSE(std::filesystem::exists(path));
}
TEST(EngineCheckpoint, ManagedSessionInitializationFailureClosesAndFreshEngineCanRetry) {
    SWMMEngine e;
    ASSERT_NO_THROW(managed1dOpen(e, "managed-failure"));
    e.context().options.rpt_controls = true;
    auto options = managedOptions(0);
    EXPECT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_ERR_IO);
    EXPECT_EQ(e.context().state, EngineState::CLOSED);
    unsigned long status = 1;
    ASSERT_EQ(swmm_checkpoint_session_status(&e, &status), SWMM_OK);
    EXPECT_EQ(status, 0);
    const auto inp = root() / "managed-failure-model.inp";
    SWMMEngine retry;
    ASSERT_EQ(retry.open(inp.string().c_str(),
                         (root() / "managed-failure-retry.rpt").string().c_str(), nullptr),
              0);
    ASSERT_EQ(swmm_checkpoint_start_session(&retry, &options), SWMM_OK)
        << swmm_checkpoint_last_error_msg();
    ASSERT_NO_THROW(step(retry));
    retry.close();
}
#ifdef OPENSWMM_HAS_2D
namespace {
/**
 * @brief Build the authored quad/source-network model for managed spatial sessions.
 *
 * Does not create files, parse or start a session. Uses fixed metric geometry/options; runtime
 * aquifer material rows are supplied after open by managedSurfaceOpen.
 *
 * @param[in] lid 0 ordinary sink, nonzero NODE LID, exact 2 Richards options.
 * @param[in] closure Compatibility parameter deliberately unused in this text builder;
 *     managedSurfaceOpen installs closure programmatically.
 * @param[in] owners Author uniform A/B surface ownership rows when true, otherwise omit them.
 * @return Owned eight-second CMS deck with one quad, two catchments, sink/outfall and LID drain
 *     route.
 */
std::string managedDeck(int lid, int closure, bool owners = true) {
    std::string d =
        "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 10/12/2026\nEND_DATE 10/12/2026\nEND_TIME 00:00:08\nROUTING_STEP .31\nVARIABLE_STEP 0\nWET_STEP 00:00:01\nDRY_STEP 00:00:01\nREPORT_STEP 00:00:02\nIGNORE_QUALITY YES\nIGNORE_SNOWMELT YES\nIGNORE_GROUNDWATER YES\n[STORAGE]\nSINK 0 100 0 FUNCTIONAL 0 0 10 0 0\n[OUTFALLS]\nDRAIN 0 FREE NO\n[RAINGAGES]\nR INTENSITY 0:00:01 1 TIMESERIES Rain\n[TIMESERIES]\nRain 10/12/2026 00:00:00 8\nRain 10/12/2026 00:00:04 0\n[SUBCATCHMENTS]\nA R B .0002 45 1 .5 0\nB R SINK .0002 65 1 .5 0\n[SUBAREAS]\nA .015 .12 .03 .05 35 PERVIOUS 55\nB .02 .13 .02 .04 25 OUTLET\n[INFILTRATION]\nA 75 5 .3\nB 75 5 .3\n[LID_CONTROLS]\nI IT\nI SURFACE 75 .05 .12 1 75\nI STORAGE 150 .667 .5 0\nI DRAIN .5 .5 0 0\n[LID_USAGE]\nA I 1 .2 .3 70 30 0 * DRAIN\n[EVAPORATION]\nCONSTANT 0\n[POLYGONS]\nA 1 0\nA 3 0\nA 3 1\nA 1 1\nB 1 1\nB 3 1\nB 3 2\nB 1 2\n[2D_OPTIONS]\nGROUNDWATER AUTO\nREPORT_2D_STEP 00:00:03\nMAX_TIMESTEP .12\nCOUPLING_SYNC .7\n[2D_VERTICES]\n0 0 0\n2 0 0\n2 2 0\n0 2 0\n[2D_QUADS]\n0 1 2 3 .03\n[2D_TRIANGLE_NODE_MAP]\n0 SINK 0 1\n[2D_AQUIFER_OPTIONS]\nMODE MESH\nNODE_ENROLMENT ROWS\nLINK_SEEPAGE NONE\nGW_ET BOUNDARY_ET\n";
    if (owners)
        d += "[2D_SURFACE_OWNERSHIP]\nA SUBCATCH UNIFORM\nB SUBCATCH UNIFORM\n";
    if (lid) {
        d.replace(d.find("SINK 0 100 0"), 12, "SINK 0 .325 0");
        d +=
            "[LID_CONTROLS]\nStack NODE\nStack SURFACE 75 .1\nStack MEDIA 150 .45 .2 .08 5 10 75\nStack AGGREGATE 100 .4 100\nStack BOTTOM 0 1\n[LID_NODES]\nSINK Stack .2\n";
        if (lid == 2)
            d +=
                "[LID_RICHARDS]\nStack OPTIONS 4 1e-7 1e-5 30\nStack 2 .03 2 1.6 .5 .0001\nStack 3 .03 2 1.6 .5 .0001\n";
    }
    return d;
}
/**
 * @brief Open a managed spatial candidate and install its aquifer profile before session start.
 *
 * Writes unchecked deck, opens then appends an authored aquifer row and fixed closure metadata to
 * the router config. Does not initialize/start or prepare output capability; public managed-session
 * call does that later. Open errors throw with partial e state retained; caller validates
 * compatible LID/port/output combinations and owns cleanup.
 *
 * @param[in,out] e Caller-owned openable engine, left opened/uninitialized after success.
 * @param[in] name Raw evidence stem below root; matching input/report/output artifacts may be
 *     overwritten.
 * @param[in] lid NODE LID selector forwarded to managedDeck.
 * @param[in] closure 2 SIGMA, 1 ENSLAVED, other values CLOSED_FORM, installed programmatically.
 * @param[in] output 1 or >=3 supplies binary destination; >=2 also sets a mesh output filename.
 * @param[in] owners Include authored ownership rows in managedDeck when true.
 * @param[in] ports Add a bottom cell-port conduit and fixed outfall backwater when true.
 */
void managedSurfaceOpen(SWMMEngine& e, const std::string& name, int lid = 0, int closure = 0,
                        int output = 0, bool owners = true, bool ports = false) {
    auto deck = managedDeck(lid, closure, owners);
    if (ports) {
        deck.replace(deck.find("DRAIN 0 FREE NO"), std::string("DRAIN 0 FREE NO").size(),
                     "DRAIN 0 FIXED .12 NO");
        deck +=
            "[CONDUITS]\nPipe SINK DRAIN 5 .015 0 0 0 0\n[XSECTIONS]\nPipe CIRCULAR .1 0 0 0\n[LID_NODE_OUTLETS]\nPipe 3 BOTTOM\n";
    }
    const auto input = root() / (name + ".inp");
    write(input, deck);
    if (e.open(input.string().c_str(), (root() / (name + ".rpt")).string().c_str(),
               output == 1 || output >= 3 ? (root() / (name + ".out")).string().c_str() : nullptr))
        throw std::runtime_error(e.context().error_message);
    auto& router = e.surfaceRouter2D();
    auto& cfg = router.aquiferConfig();
    cfg.options.authored = true;
    cfg.options.force_closed_form = true;
    cfg.options.closure = closure == 2   ? twoD::GwClosure::SIGMA
                          : closure == 1 ? twoD::GwClosure::ENSLAVED
                                         : twoD::GwClosure::CLOSED_FORM;
    twoD::GwAquiferRow row;
    row.Ks = .0036 * 1000;
    row.zs = 2;
    row.hg0 = 1;
    row.alpha = 2;
    cfg.rows.push_back(row);
    if (output >= 2)
        router.options().output_file = (root() / (name + ".h5")).string();
}
/**
 * @brief Resolve the current candidate surface-ownership qualification token.
 *
 * Throws runtime_error with the first preview error when invalid; assumes invalid preview supplies
 * an error entry. Does not mutate model/mesh, retain preview references or start a checkpoint
 * session.
 *
 * @param[in] e Borrowed opened model with valid router mesh/options/aquifer ownership rows.
 * @return Owned resolved ownership token when preview is valid.
 */
std::string managedToken(SWMMEngine& e) {
    auto& r = e.surfaceRouter2D();
    auto p = twoD::resolveSurfaceOwnership(e.context(), r.mesh(), r.options(), r.aquiferConfig(),
                                           r.aquiferConfig().surface_owners);
    if (!p.valid())
        throw std::runtime_error(p.errors.front());
    return p.token;
}
}
TEST(EngineCheckpoint, ManagedSessionReviewedGraphsCoverLidsAquifersAndExactRestart) {
    for (int lid : {0, 1, 2})
        for (int closure : {0, 1, 2}) {
            const auto name =
                "managed-surface-" + std::to_string(lid) + "-" + std::to_string(closure);
            SWMMEngine a, b;
            ASSERT_NO_THROW(managedSurfaceOpen(a, name + "-a", lid, closure));
            ASSERT_NO_THROW(managedSurfaceOpen(b, name + "-b", lid, closure));
            const auto ta = managedToken(a), tb = managedToken(b),
                       ja = (root() / (name + "-a.history")).string(),
                       jb = (root() / (name + "-b.history")).string();
            const auto oa = managedOptions(0, ja.c_str(), ta.c_str(), true),
                       ob = managedOptions(0, jb.c_str(), tb.c_str(), true);
            ASSERT_EQ(swmm_checkpoint_start_session(&a, &oa), SWMM_OK)
                << swmm_checkpoint_last_error_msg();
            ASSERT_EQ(swmm_checkpoint_start_session(&b, &ob), SWMM_OK)
                << swmm_checkpoint_last_error_msg();
            ASSERT_NE(a.sourceDelivery(), nullptr);
            ASSERT_NE(b.sourceDelivery(), nullptr);
            ASSERT_NO_THROW(until(a, 4.2));
            const auto cp = root() / (name + ".chk");
            ASSERT_EQ(swmm_checkpoint_save(&a, cp.string().c_str()), SWMM_OK);
            CheckpointInspection info;
            ASSERT_EQ(b.inspectCheckpoint(cp.string(), info), "");
            EXPECT_TRUE(info.completed_source_seconds);
            EXPECT_FALSE(info.driver);
            EXPECT_TRUE(info.detailed_sources);
            ASSERT_EQ(swmm_checkpoint_restore(&b, cp.string().c_str()), SWMM_OK);
            compare(a, b, name + "-restored");
            ASSERT_NO_THROW(until(a, 6.2));
            ASSERT_NO_THROW(until(b, 6.2));
            compare(a, b, name + "-continued");
            const auto& da = a.sourceDelivery()->delivered();
            const auto& db = b.sourceDelivery()->delivered();
            ASSERT_EQ(da.size(), db.size());
            for (size_t n = 0; n < da.size(); ++n) {
                EXPECT_EQ(da[n].runoff, db[n].runoff);
                EXPECT_EQ(da[n].drain, db[n].drain);
                EXPECT_EQ(da[n].rate, db[n].rate);
            }
            a.end();
            b.end();
            a.close();
            b.close();
        }
}
TEST(EngineCheckpoint,
     ManagedSessionKeepsOrdinaryGuardAndRefusesStaleOrMissingReviewBeforeWriters) {
    SWMMEngine ordinary;
    ASSERT_NO_THROW(managedSurfaceOpen(ordinary, "managed-ordinary"));
    EXPECT_NE(ordinary.initialize(), 0);
    ordinary.close();
    SWMMEngine e;
    ASSERT_NO_THROW(managedSurfaceOpen(e, "managed-stale"));
    const auto token = managedToken(e);
    auto options = managedOptions(0);
    EXPECT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_ERR_IO);
    EXPECT_EQ(e.context().state, EngineState::OPENED);
    e.context().subcatches.width[0] +=
        1; // width is validated by the native model, not the geometric token
    e.context().subcatches.frac_imperv[0] += .01;
    options.ownership_token = token.c_str();
    EXPECT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_ERR_IO);
    EXPECT_EQ(e.context().state, EngineState::OPENED);
    EXPECT_FALSE(std::filesystem::exists(root() / "managed-stale.rpt"));
    const auto current = managedToken(e);
    options.ownership_token = current.c_str();
    ASSERT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_OK)
        << swmm_checkpoint_last_error_msg();
}
TEST(EngineCheckpoint, ManagedSessionMeshOnlyOwnsItsStageAndRefusesUnqualifiedJournal) {
    SWMMEngine e;
    ASSERT_NO_THROW(managedSurfaceOpen(e, "managed-mesh-only", 0, 0, 0, false));
    const auto journal = (root() / "managed-mesh-only.history").string();
    auto options = managedOptions(0, journal.c_str());
    EXPECT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_ERR_IO);
    EXPECT_EQ(e.context().state, EngineState::OPENED);
    options.source_history = nullptr;
    ASSERT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_OK)
        << swmm_checkpoint_last_error_msg();
    ASSERT_NO_THROW(step(e));
    const auto cp = root() / "managed-mesh-only.chk";
    ASSERT_EQ(swmm_checkpoint_save(&e, cp.string().c_str()), SWMM_OK);
    e.close();
}
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
TEST(EngineCheckpoint, ManagedSessionAllOutputProfilesAndRelocatedFileBundleRecover) {
    for (int mode : {1, 2, 3, 4}) {
        const auto name = "managed-output-" + std::to_string(mode);
        SWMMEngine a, b;
        ASSERT_NO_THROW(managedSurfaceOpen(a, name + "-a", 2, 2, mode));
        ASSERT_NO_THROW(managedSurfaceOpen(b, name + "-b", 2, 2, mode));
        const auto ta = managedToken(a), tb = managedToken(b),
                   ja = (root() / (name + "-a.history")).string(),
                   jb = (root() / (name + "-b.history")).string();
        const auto oa = managedOptions(mode, ja.c_str(), ta.c_str(), true),
                   ob = managedOptions(mode, jb.c_str(), tb.c_str(), true);
        ASSERT_EQ(swmm_checkpoint_start_session(&a, &oa), SWMM_OK)
            << swmm_checkpoint_last_error_msg();
        ASSERT_EQ(swmm_checkpoint_start_session(&b, &ob), SWMM_OK)
            << swmm_checkpoint_last_error_msg();
        ASSERT_NO_THROW(until(a, 4.2));
        auto cp = root() / (name + ".chk");
        ASSERT_EQ(swmm_checkpoint_save(&a, cp.string().c_str()), SWMM_OK);
        if (mode == 4) {
            const auto folder = root() / (name + "-bundle");
            ASSERT_EQ(swmm_checkpoint_export_bundle(cp.string().c_str(), folder.string().c_str()),
                      SWMM_OK);
            cp = folder / "session.chk";
        }
        PublicInspection info;
        ASSERT_EQ(swmm_checkpoint_inspect(&b, cp.string().c_str(), &info.handle), SWMM_OK)
            << swmm_checkpoint_last_error_msg();
        ASSERT_EQ(swmm_checkpoint_restore(&b, cp.string().c_str()), SWMM_OK);
        if (mode == 2 || mode == 3)
            compareMesh(a, b, name + "-restored");
        else
            compare(a, b, name + "-restored");
        ASSERT_NO_THROW(until(a, 6.2));
        ASSERT_NO_THROW(until(b, 6.2));
        if (mode == 2 || mode == 3)
            compareMesh(a, b, name + "-continued");
        else
            compare(a, b, name + "-continued");
        ASSERT_EQ(a.end(), 0);
        ASSERT_EQ(b.end(), 0);
        if (mode == 1 || mode >= 3)
            sameBinary(name + "-a", name + "-b", 3);
        if (mode >= 2)
            sameMesh(name + "-a", name + "-b", 3);
        a.close();
        b.close();
    }
}
#endif
#endif
TEST(EngineCheckpoint, ManagedSessionRefusesAncillaryWritersBeforeAnyOutputIsTouched) {
    SWMMEngine e;
    ASSERT_NO_THROW(managed1dOpen(e, "managed-ancillary"));
    const auto target = root() / "managed-ancillary-interface.out";
    write(target, "protected ancillary output");
    e.context().files.runoff_path.original = target.string();
    e.context().files.runoff_mode = FileMode::SAVE;
    const auto options = managedOptions();
    EXPECT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_ERR_IO);
    EXPECT_EQ(e.context().state, EngineState::OPENED);
    EXPECT_EQ(bytes(target), "protected ancillary output");
    EXPECT_FALSE(std::filesystem::exists(root() / "managed-ancillary.rpt"));
    e.context().files.runoff_mode = FileMode::NONE;
    e.context().files.runoff_path = {};
    e.context().lid_usage.rpt_file.push_back(target.string());
    EXPECT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_ERR_IO);
    EXPECT_EQ(e.context().state, EngineState::OPENED);
    EXPECT_EQ(bytes(target), "protected ancillary output");
}
#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint,
     ManagedSessionUsesPersistedReviewedInputAndExactAreasWithoutInternalAttachments) {
    for (int lid : {0, 1, 2})
        for (int closure : {0, 1, 2}) {
            const auto name =
                "managed-authored-" + std::to_string(lid) + "-" + std::to_string(closure);
            SWMMEngine author;
            ASSERT_NO_THROW(managedSurfaceOpen(author, name + "-author", lid, closure));
            const auto inp = root() / (name + "-persisted.inp");
            ASSERT_EQ(swmm_model_write(&author, inp.string().c_str()), SWMM_OK);
            author.close();
            SWMMEngine a, b;
            ASSERT_EQ(a.open(inp.string().c_str(), (root() / (name + "-a.rpt")).string().c_str(),
                             nullptr),
                      0);
            ASSERT_EQ(b.open(inp.string().c_str(), (root() / (name + "-b.rpt")).string().c_str(),
                             nullptr),
                      0);
            const auto ta = managedToken(a), tb = managedToken(b);
            const auto oa = managedOptions(0, nullptr, ta.c_str()),
                       ob = managedOptions(0, nullptr, tb.c_str());
            ASSERT_EQ(swmm_checkpoint_start_session(&a, &oa), SWMM_OK)
                << swmm_checkpoint_last_error_msg();
            ASSERT_EQ(swmm_checkpoint_start_session(&b, &ob), SWMM_OK)
                << swmm_checkpoint_last_error_msg();
            EXPECT_EQ(a.surfaceRouter2D().subsurface().state().closure[0], int8_t(closure));
            ASSERT_NE(a.sourceDelivery(), nullptr);
            ASSERT_NO_THROW(until(a, 4.2));
            const auto cp = root() / (name + ".chk");
            ASSERT_EQ(swmm_checkpoint_save(&a, cp.string().c_str()), SWMM_OK);
            ASSERT_EQ(swmm_checkpoint_restore(&b, cp.string().c_str()), SWMM_OK);
            ASSERT_NO_THROW(until(a, 6.2));
            ASSERT_NO_THROW(until(b, 6.2));
            compare(a, b, name + "-continued");
            a.close();
            b.close();
        }
}
#endif
TEST(EngineCheckpoint, ManagedSessionStatusDoesNotLeakIntoALaterManualAttachment) {
    SWMMEngine author;
    ASSERT_NO_THROW(start(author, "managed-status-model", 0, 0, false, false));
    author.close();
    SWMMEngine e;
    const auto inp = (root() / "managed-status-model.inp").string();
    ASSERT_EQ(e.open(inp.c_str(), (root() / "managed-status-first.rpt").string().c_str(), nullptr),
              0);
    const auto options = managedOptions();
    ASSERT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_OK);
    e.close();
    ASSERT_EQ(e.open(inp.c_str(), (root() / "managed-status-manual.rpt").string().c_str(), nullptr),
              0);
    ASSERT_EQ(e.initialize(), 0);
    ASSERT_EQ(e.start(0), 0);
    ASSERT_EQ(e.enableCheckpointing(), "");
    unsigned long flags = 0;
    ASSERT_EQ(swmm_checkpoint_session_status(&e, &flags), SWMM_OK);
    EXPECT_TRUE(flags & SWMM_CHECKPOINT_SESSION_ATTACHED);
    EXPECT_FALSE(flags & SWMM_CHECKPOINT_SESSION_MANAGED);
    ASSERT_NO_THROW(step(e));
    e.close();
}
TEST(EngineCheckpoint,
     ManagedSessionExclusiveReportRefusesLateCollisionsAndFallbackDoesNotTruncate) {
    const auto path = root() / "managed-late-report.rpt";
    DefaultReportPlugin report(path.string());
    SimulationContext context;
    ASSERT_EQ(report.initialize({}, nullptr), 0);
    ASSERT_EQ(report.validate(context), 0);
    report.requireFreshOutput();
    write(path, "late report owner");
    EXPECT_NE(report.prepare(context), 0);
    EXPECT_EQ(bytes(path), "late report owner");
    EXPECT_NE(report.write_summary(context), 0);
    EXPECT_EQ(bytes(path), "late report owner");
    std::filesystem::remove(path);
    ASSERT_EQ(report.prepare(context), 0);
    ASSERT_EQ(report.write_summary(context), 0);
    EXPECT_GT(bytes(path).size(), 0);
}

TEST(EngineCheckpoint, LidBottomOwnerParticipatesInCompatibilityAndSealedContinuation) {
    for (int lid : {1, 2}) {
        auto name = "bottom-owner-" + std::to_string(lid);
        SWMMEngine a, b;
        ASSERT_NO_THROW(start(a, name + "-a", lid, 0, false));
        const auto good = root() / (name + "-current.chk");
        ASSERT_EQ(a.saveCheckpoint(good.string()), "");
        a.context().node_subtypes.storages.lid[0].bottom = LidNodeBottom::Sealed;
        EXPECT_FALSE(a.saveCheckpoint((root() / (name + "-changed.chk")).string()).empty());
        ASSERT_NO_THROW(start(b, name + "-b", lid, 0, false, false));
        b.context().node_subtypes.storages.lid[0].bottom = LidNodeBottom::Sealed;
        ASSERT_EQ(b.enableCheckpointing(), "");
        EXPECT_FALSE(b.loadCheckpoint(good.string()).empty());
        SWMMEngine c;
        ASSERT_NO_THROW(start(c, name + "-c", lid, 0, false, false));
        c.context().node_subtypes.storages.lid[0].bottom = LidNodeBottom::Sealed;
        ASSERT_EQ(c.enableCheckpointing(), "");
        for (int k = 0; k < 4; ++k)
            ASSERT_NO_THROW(step(b));
        auto sealed = root() / (name + "-sealed.chk");
        ASSERT_EQ(b.saveCheckpoint(sealed.string()), "");
        ASSERT_EQ(c.loadCheckpoint(sealed.string()), "");
        for (int k = 0; k < 6; ++k) {
            ASSERT_NO_THROW(step(b));
            ASSERT_NO_THROW(step(c));
            compare(b, c, name);
        }
    }
}

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, ManagedLidBottomsUseGlobalRoutingAndResumePendingGroundwaterWater) {
    for (int model : {1, 2})
        for (int closure : {0, 1, 2})
            for (bool owners : {false, true})
                for (bool ports : {false, true}) {
                    const auto name = "managed-lid-bed-" + std::to_string(model) + "-" +
                                      std::to_string(closure) + "-" + std::to_string(owners) + "-" +
                                      std::to_string(ports);
                    SCOPED_TRACE(name);
                    SWMMEngine a, b;
                    auto setup = [&](SWMMEngine& e, const std::string& suffix) {
                        managedSurfaceOpen(e, name + suffix, model, closure, 0, owners, ports);
                        auto& c = e.context();
                        c.node_subtypes.storages.lid[0].bottom = LidNodeBottom::MeshAquifer;
                        twoD::GwNodeBed bed;
                        bed.node = c.node_names.find("SINK");
                        bed.cell = 0;
                        bed.area = .5;
                        bed.Kc = 5;
                        bed.dC = .1;
                        e.surfaceRouter2D().aquiferConfig().node_beds.push_back(bed);
                        e.surfaceRouter2D().aquiferNodeNames().push_back("SINK");
                    };
                    ASSERT_NO_THROW(setup(a, "-a"));
                    ASSERT_NO_THROW(setup(b, "-b"));
                    const auto ta = owners ? managedToken(a) : "",
                               tb = owners ? managedToken(b) : "";
                    const auto ja = (root() / (name + "-a.history")).string(),
                               jb = (root() / (name + "-b.history")).string();
                    const auto oa = managedOptions(0, ja.c_str(), ta.c_str(), closure != 0),
                               ob = managedOptions(0, jb.c_str(), tb.c_str(), closure != 0);
                    ASSERT_EQ(swmm_checkpoint_start_session(&a, &oa), SWMM_OK)
                        << swmm_checkpoint_last_error_msg();
                    ASSERT_EQ(swmm_checkpoint_start_session(&b, &ob), SWMM_OK)
                        << swmm_checkpoint_last_error_msg();
                    ASSERT_NO_THROW(until(a, 2.2));
                    const auto path = root() / (name + ".chk");
                    ASSERT_EQ(a.saveCheckpoint(path.string()), "");
                    ASSERT_EQ(b.loadCheckpoint(path.string()), "");
                    compare(a, b, name + "-restored");
                    ASSERT_NO_THROW(until(a, 6.2));
                    ASSERT_NO_THROW(until(b, 6.2));
                    compare(a, b, name + "-continued");
                    const auto& ga = a.surfaceRouter2D().subsurface();
                    const auto& gb = b.surfaceRouter2D().subsurface();
                    SWMM_LidBottomState diagnostic{};
                    ASSERT_EQ(swmm_lid_node_bottom_state_get(
                                  &a, a.context().node_names.find("SINK"), &diagnostic),
                              SWMM_OK);
                    EXPECT_EQ(diagnostic.flow_model, model == 2 ? 1 : 0);
                    EXPECT_TRUE(diagnostic.valid_sample);
                    EXPECT_NEAR(diagnostic.net_cumulative_m3,
                                ga.richardsBedDownCumulative()[0] - ga.richardsBedUpCumulative()[0],
                                1e-14);
                    EXPECT_LE(diagnostic.accepted_down_m3, diagnostic.reserved_down_m3);
                    EXPECT_LE(diagnostic.accepted_up_m3, diagnostic.reserved_up_m3);
                    EXPECT_EQ(ga.pendingLidWater(), gb.pendingLidWater());
                    EXPECT_EQ(ga.richardsBedDownCumulative(), gb.richardsBedDownCumulative());
                    EXPECT_EQ(bytes(ja), bytes(jb));
                    EXPECT_EQ(ga.nodeRefunds(), 0);
                    EXPECT_NEAR(ga.state().continuityResidual(), 0, 1e-10);
                    EXPECT_EQ(a.context().node_subtypes.storages.exfil_loss[0], 0);
                    if (ports)
                        EXPECT_LT(a.context().links.flow[0], 0)
                            << "Backwater must exercise the native LID outlet.";
                    if (model == 2)
                        EXPECT_GT(
                            ga.richardsBedDownCumulative()[0] + ga.richardsBedUpCumulative()[0], 0);
                    a.end();
                    b.end();
                    a.close();
                    b.close();
                }
}
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
TEST(EngineCheckpoint, LidOnlyDetailedHistoryPreservesProfilesAndDirectionalWater) {
    for (int model : {1, 2}) {
        const auto name = "lid-only-history-" + std::to_string(model);
        SWMMEngine e;
        ASSERT_NO_THROW(managedSurfaceOpen(e, name, model, 0, 0, false));
        auto& c = e.context();
        c.node_subtypes.storages.lid[0].bottom = LidNodeBottom::MeshAquifer;
        twoD::GwNodeBed bed;
        bed.node = c.node_names.find("SINK");
        bed.cell = 0;
        bed.area = .5;
        bed.Kc = 5;
        bed.dC = .1;
        e.surfaceRouter2D().aquiferConfig().node_beds.push_back(bed);
        e.surfaceRouter2D().aquiferNodeNames().push_back("SINK");
        const auto options = managedOptions();
        ASSERT_EQ(swmm_checkpoint_start_session(&e, &options), SWMM_OK)
            << swmm_checkpoint_last_error_msg();
        twoD::EngineSourceResultHistory history;
        ASSERT_EQ(e.setEngineSourceResultHistory(history, true), "");
        ASSERT_NO_THROW(until(e, 2.2));
        const auto path = (root() / (name + ".h5")).string();
        ASSERT_EQ(history.save(path), "");
        twoD::EngineSourceResultHistory loaded;
        bool available = false;
        ASSERT_EQ(twoD::EngineSourceResultHistory::read(path, loaded, available), "");
        ASSERT_TRUE(available);
        EXPECT_EQ(loaded.tables().at("source_water").rows(), 0u);
        EXPECT_GT(loaded.tables().at("lid_profile").rows(), 0u);
        EXPECT_EQ(loaded.tables().at("richards_profile").rows() > 0, model == 2);
        for (const auto& [key, table] : history.tables())
            EXPECT_EQ(table.columns, loaded.tables().at(key).columns) << key;
        ASSERT_NE(loaded.sourceIntervals(), nullptr);
        EXPECT_GT(loaded.sourceIntervals()->tables().at("receiver").rows(), 0u);
        e.end();
        e.close();
    }
}
#endif
TEST(EngineCheckpoint, ManagedSnowOwnersAndMeshResumeAcceptedWaterWithoutDoubleCounting) {
    for (int output : {0, 2})
        for (bool owners : {false, true})
            for (auto model : {surface::SnowKind::DegreeDay, surface::SnowKind::Ueb}) {
                const auto name = "managed-snow-" + std::to_string(owners) + "-" +
                                  std::to_string(int(model)) + "-" + std::to_string(output);
                SWMMEngine a, b;
                SCOPED_TRACE(name);
                const auto setup = [&](SWMMEngine& e, const std::string& suffix) {
                    managedSurfaceOpen(e, name + suffix, 0, 0, output, owners);
                    auto& c = e.context();
                    c.options.ignore_snow_melt = false;
                    c.snowpack_names.try_add("Snow");
                    c.snowpacks.names = {"Snow"};
                    c.snowpacks.plowable = c.snowpacks.impervious =
                        c.snowpacks.pervious = {std::array<double, 7>{1, 2, 0, .1, 100, 10, 0}};
                    c.snowpacks.removal = {std::array<double, 6>{}};
                    c.snowpacks.removal_subcatch = {""};
                    c.snowpacks.config.model = model;
                    c.snowpacks.config.step = 60;
                    c.snowpacks.config.monthly_range.fill(10.);
                    c.snowpacks.config.site = surface::ClimateSite{45, -120, -8, 1000};
                    if (owners)
                        for (int sc = 0; sc < c.n_subcatches(); ++sc)
                            c.subcatches.snowpack[sc] = 0;
                    else
                        c.snowpacks.mesh_config.defaults["*"].pack = "Snow";
                };
                ASSERT_NO_THROW(setup(a, "-a"));
                ASSERT_NO_THROW(setup(b, "-b"));
                const auto ta = owners ? managedToken(a) : "", tb = owners ? managedToken(b) : "";
                const auto oa = managedOptions(output, nullptr, ta.c_str()),
                           ob = managedOptions(output, nullptr, tb.c_str());
                ASSERT_EQ(swmm_checkpoint_start_session(&a, &oa), SWMM_OK)
                    << swmm_checkpoint_last_error_msg();
                ASSERT_EQ(swmm_checkpoint_start_session(&b, &ob), SWMM_OK)
                    << swmm_checkpoint_last_error_msg();
                ASSERT_NO_THROW(until(a, 2.2));
                const auto path = root() / (name + ".chk");
                ASSERT_EQ(a.saveCheckpoint(path.string()), "");
                ASSERT_EQ(b.loadCheckpoint(path.string()), "");
                ASSERT_NO_THROW(until(a, 6.2));
                ASSERT_NO_THROW(until(b, 6.2));
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
                if (output == 2)
                    compareMesh(a, b, name + "-continued");
                else
#endif
                    compare(a, b, name + "-continued");
                if (owners) {
                    const auto* source = a.completedSourceDriver();
                    ASSERT_NE(source, nullptr);
                    ASSERT_TRUE(source->snow().active());
                    double stored = 0;
                    for (const auto& book : source->snow().ledgers()) {
                        EXPECT_NEAR(book.residual(), 0, 1e-9);
                        stored += book.stored;
                    }
                    EXPECT_NEAR(a.context().mass_balance.runoff_final_snow, stored, 1e-9);
                    SWMM_SnowState state{};
                    ASSERT_EQ(swmm_subcatch_get_snow_process_state(&a, 0, 2, &state), SWMM_OK);
                    EXPECT_EQ(state.swe_m,
                              (source->snow().state().wsnow[2] + source->snow().state().fw[2]) *
                                  .3048);
                    auto seed_context = a.context();
                    snow::SnowSolver seed;
                    ASSERT_EQ(surface::initializeNativeSnow(seed_context, seed), "");
                    const auto hot = a.context().state_accessors.get_snow_state();
                    ASSERT_EQ(surface::restoreCombinedSnowCheckpoint(seed_context, seed.state(),
                                                                     nullptr, hot, true, true),
                              "");
                    EXPECT_EQ((seed.state().wsnow[2] + seed.state().fw[2]) * .3048, state.swe_m);
                    if (model == surface::SnowKind::Ueb) {
                        EXPECT_EQ(
                            seed_context.snowpacks.runtime.columns[2].accepted.point.energy,
                            source->context().snowpacks.runtime.columns[2].accepted.point.energy);
                        EXPECT_FALSE(seed_context.snowpacks.runtime.columns[2].pending());
                    }
                    if (model == surface::SnowKind::Ueb) {
                        SWMM_SnowLedger book{};
                        ASSERT_EQ(swmm_snow_get_ueb_ledger(&a, &book), SWMM_OK);
                        EXPECT_NEAR(book.residual_m3, 0, 1e-9);
                        EXPECT_NEAR(book.energy_residual_kj, 0, 1e-6);
                    }
                } else {
                    const auto& snow = a.surfaceRouter2D().state().snow;
                    ASSERT_TRUE(snow.active());
                    for (const auto& book : snow.ledgers()) {
                        EXPECT_NEAR(book.residual(), 0, 1e-9);
                        if (model == surface::SnowKind::Ueb)
                            EXPECT_NEAR(book.energyResidual(), 0, 1e-6);
                    }
                    EXPECT_EQ(snow.saveCheckpoint(),
                              b.surfaceRouter2D().state().snow.saveCheckpoint());
                }

#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
                if (output == 2) {
                    ASSERT_EQ(a.end(), 0);
                    ASSERT_EQ(b.end(), 0);
                    hid_t f = H5Fopen((root() / (name + "-a.h5")).string().c_str(), H5F_ACC_RDONLY,
                                      H5P_DEFAULT);
                    ASSERT_GE(f, 0);
                    hid_t d = H5Dopen2(f, "time", H5P_DEFAULT), space = H5Dget_space(d);
                    const auto periods = H5Sget_simple_extent_npoints(space);
                    H5Sclose(space);
                    H5Dclose(d);
                    H5Fclose(f);
                    sameMesh(name + "-a", name + "-b", int(periods));
                }
#endif
            }
}
TEST(EngineCheckpoint, UebGroundContactRestoresAcceptedEnergyExactly) {
    const std::string name = "ueb-ground-contact-restart";
    ThermalSurfaceSession a, b;
    ASSERT_NO_THROW(a.start(name + "-a", 8, "channel:test", true, false, false, false, false, false,
                            true, true));
    ASSERT_NO_THROW(b.start(name + "-b", 8, "channel:test", true, false, false, false, false, false,
                            true, true));
    auto& sa = a.engine.surfaceRouter2D().state().snow;
    auto& sb = b.engine.surfaceRouter2D().state().snow;
    ASSERT_TRUE(sa.active());
    ASSERT_TRUE(sb.active());
    ASSERT_EQ(sa.kind(0), surface::SnowKind::Ueb);
    ASSERT_EQ(sb.kind(0), surface::SnowKind::Ueb);
    ASSERT_GT(sa.outputs()[0].cover_frac, 0);
    ASSERT_GT(sb.outputs()[0].cover_frac, 0);
    auto& ground = a.engine.surfaceRouter2D().subsurface().heat();
    auto& resumed_ground = b.engine.surfaceRouter2D().subsurface().heat();
    ASSERT_TRUE(a.engine.surfaceRouter2D().state().heat.enabled);
    ASSERT_GE(a.engine.surfaceRouter2D().state().transport.temp_row, 0);
    ASSERT_TRUE(ground.active());
    ASSERT_TRUE(resumed_ground.active());
    ASSERT_TRUE(ground.config->options.conduction);
    ASSERT_TRUE(resumed_ground.config->options.conduction);
    ASSERT_FALSE(ground.cells[0].column.energy.empty());
    ASSERT_FALSE(resumed_ground.cells[0].column.energy.empty());
    ASSERT_TRUE(sa.groundContactEnabled(0));
    ASSERT_TRUE(sb.groundContactEnabled(0));
    ASSERT_TRUE(ground.snowContactEnabled(0));
    ASSERT_TRUE(resumed_ground.snowContactEnabled(0));
    EXPECT_GT(std::abs(sa.outputs()[0].Ts_C - ground.activeTemperature(0, 0)), 1e-6)
        << "snow surface=" << sa.outputs()[0].Ts_C << " soil=" << ground.activeTemperature(0, 0);
    while (a.engine.context().current_time < 43.1)
        ASSERT_NO_THROW(step(a.engine));
    EXPECT_EQ(ground.cells[0].failed, 0) << ground.cells[0].error;
    EXPECT_NE(ground.cells[0].surface_j, 0);
    ASSERT_NE(sa.ledgers()[0].ground_contact_energy, 0);
    EXPECT_NEAR(sa.ledgers()[0].energyResidual(), 0, 1e-6);
    const auto path = root() / (name + ".chk");
    ASSERT_EQ(a.engine.saveCheckpoint(path.string()), "");
    ASSERT_EQ(b.engine.loadCheckpoint(path.string()), "");
    compare(a.engine, b.engine, name + "-restored");
    while (a.engine.context().current_time < 83.1)
        ASSERT_NO_THROW(step(a.engine));
    while (b.engine.context().current_time < 83.1)
        ASSERT_NO_THROW(step(b.engine));
    compare(a.engine, b.engine, name + "-continued");
    EXPECT_EQ(sa.saveCheckpoint(), sb.saveCheckpoint());
    EXPECT_EQ(ground.cells[0].column.energy, resumed_ground.cells[0].column.energy);
    EXPECT_EQ(ground.cells[0].surface_j, resumed_ground.cells[0].surface_j);
    EXPECT_NEAR(sa.ledgers()[0].energyResidual(), 0, 1e-6);
}
#endif

#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
namespace {
/**
 * @brief Start the reactive ground/heat profile with selected aqueous and thermal layers.
 *
 * Authors full-coupling RK5 SEC reaction rows in saturated/unsaturated zones, sorption, nonuniform
 * seeds and a 1e-6 m3/s groundwater source with chemistry/age/temperature. Calls thermal setup with
 * lateral conduction, heterogeneous ground and ground-column quality. Raw tokens are unescaped and
 * incompatible units/counts may reject downstream; caller retains partial session state on
 * exceptions.
 *
 * @param[in,out] session Fresh owned thermal session; extra_input is replaced before start.
 * @param[in] name Raw root-relative evidence stem.
 * @param[in] chemistry Zero selects bulk CLOSED_FORM with M_LAYERS=4; nonzero selects layered SIGMA
 *     and that authored count, supporting layer-two seed.
 * @param[in] thermal Authored thermal-layer count copied without validation.
 * @param[in] output Enable mesh HDF5 checkpoint output when true.
 * @param[in] rate Raw first-order A-to-B rate coefficient expression token in 1/s.
 * @param[in] unit Raw mass-unit keyword for species A; B remains MG.
 */
void startReactiveGroundSession(ThermalSurfaceSession& session, const std::string& name,
                                int chemistry, int thermal, bool output = true,
                                const std::string& rate = ".001", const std::string& unit = "MG") {
    session.extra_input =
        "\n[OPTIONS]\nSTART_DATE 01/31/2026\nSTART_TIME 23:59:30\nEND_DATE 02/01/2026\nEND_TIME 00:02:30\n"
        "REPORT_START_DATE 01/31/2026\nREPORT_START_TIME 23:59:30\nREPORT_STEP 00:00:07\n"
        "[2D_OPTIONS]\nTRANSPORT_MSX YES\nREPORT_2D_VARIABLES DEPTH HEAT GROUNDWATER\n"
        "[2D_AQUIFER_OPTIONS]\nM_LAYERS " +
        std::to_string(chemistry ? chemistry : 4) + "\n[GW_TRANSPORT_OPTIONS]\nUNSAT_TRANSPORT " +
        (chemistry ? std::string("LAYERED") : std::string("BULK")) + "\nTHERMAL_LAYERS " +
        std::to_string(thermal) +
        "\n[REACTION_OPTIONS]\nSOLVER RK5\nRATE_UNITS SEC\nCOUPLING FULL\nATOL 1e-13\nRTOL 1e-11\n"
        "[REACTION_SPECIES]\nBULK A " +
        unit +
        "\nBULK B MG\n[REACTION_SUBSURFACE]\n"
        "* SAT A RATE -" +
        rate + "*A\n* SAT B RATE " + rate + "*A\n* UNSAT A RATE -" + rate + "*A\n* UNSAT B RATE " +
        rate +
        "*A\n"
        "[GW_SORPTION]\n* A 2 0\n* B 2 0\n[GW_INITIAL_QUALITY]\n* SAT A 7\n* UNSAT A 11\n"
        "[GW_SOURCES]\nW * FLOW .000001 A CONC 3 B CONC 1 Dye CONC 3 __WATER_AGE__ CONC 2 __TEMPERATURE__ CONC -4\n";
    if (chemistry)
        session.extra_input += "[GW_INITIAL_QUALITY]\n* LAYER 2 A 23\n";
    session.start(name, 12, "channel:test", false, false, false, true, true, false, true, false,
                  false, output, false, false, true, chemistry ? "SIGMA" : "CLOSED_FORM");
}
/**
 * @brief Sum selected conserved-pair groundwater/surface amount and deep export.
 *
 * No unit normalization, reaction-equivalence conversion or row bounds checking is performed.
 * Requires the intended conserved species-pair layout; does not include arbitrary other export
 * ledgers or mutate state.
 *
 * @param[in] engine Borrowed initialized reactive mesh/GW engine.
 * @param[in] a Valid common transport-row index in GW ledgers and surface mass layout.
 * @param[in] b Second valid row index in the same layout, with compatible native
 *     concentration-volume units.
 * @return Sum of groundwater ledgered storage, deep-loss exports and surface cell mass for a/b in
 *     native transport amount units.
 */
double reactiveGroundAmount(SWMMEngine& engine, int a, int b) {
    const auto& router = engine.surfaceRouter2D();
    const auto& tr = router.subsurface().transport();
    const auto& surface = router.state().transport;
    double amount =
        tr.ledgeredStorage(a) + tr.ledgeredStorage(b) + tr.lost_deep[a] + tr.lost_deep[b];
    for (int cell = 0; cell < surface.n_cells; ++cell)
        amount += surface.cell_mass[surface.idx(a, cell)] + surface.cell_mass[surface.idx(b, cell)];
    return amount;
}
}
TEST(EngineCheckpoint, MeshGroundReactionsPreservePendingContactsLedgersAndOutputAcrossMidnight) {
    for (const auto grids : {std::pair<int, int>{0, 4}, {5, 3}, {3, 7}})
        for (double boundary : {7.1, 31.1}) {
            const auto name = "reactive-ground-" + std::to_string(grids.first) + "-" +
                              std::to_string(grids.second) + "-" + std::to_string(boundary);
            SCOPED_TRACE(name);
            ThermalSurfaceSession source, target;
            ASSERT_NO_THROW(
                startReactiveGroundSession(source, name + "-a", grids.first, grids.second));
            ASSERT_NO_THROW(
                startReactiveGroundSession(target, name + "-b", grids.first, grids.second));
            auto& gw = source.engine.surfaceRouter2D().subsurface();
            const auto& tr = gw.transport();
            const int a = tr.rowIndex("A"), b = tr.rowIndex("B");
            ASSERT_GE(a, 0);
            ASSERT_GE(b, 0);
            const double initial = reactiveGroundAmount(source.engine, a, b);
            std::vector<double> composition(tr.n_species, 0);
            composition[a] = 3;
            composition[b] = 1;
            composition[tr.rowIndex("Dye")] = 7;
            composition[tr.age_row] = 2;
            composition[tr.temp_row] = -4;
            const double elevation = gw.state().z_bed[0] + gw.state().zs[0] - .05;
            gw.bookLinkContact(0, .002, composition, elevation, elevation);
            while (source.engine.context().current_time < boundary)
                ASSERT_NO_THROW(step(source.engine));
            gw.bookLinkContact(0, .00025, composition, elevation, elevation);
            ASSERT_GT(gw.state().lacc[0], 0);
            const auto saved = root() / (name + ".chk");
            ASSERT_EQ(source.engine.saveCheckpoint(saved.string()), "");
            ASSERT_EQ(target.engine.loadCheckpoint(saved.string()), "");
            compareMesh(source.engine, target.engine, name + "-restored");
            EXPECT_EQ(tr.generated_age,
                      target.engine.surfaceRouter2D().subsurface().transport().generated_age);
            while (source.engine.context().current_time < 83.1) {
                ASSERT_NO_THROW(step(source.engine));
                ASSERT_NO_THROW(step(target.engine));
            }
            compareMesh(source.engine, target.engine, name + "-continued");
            EXPECT_GT(tr.lost_reaction[a], 0);
            EXPECT_LT(tr.lost_reaction[b], 0);
            EXPECT_GT(tr.gained_source[a], 0);
            EXPECT_NEAR(reactiveGroundAmount(source.engine, a, b),
                        initial + .00225 * 4 + tr.gained_source[a] + tr.gained_source[b],
                        1e-10 * std::max(1., initial));
            for (int row : {a, b, tr.age_row})
                EXPECT_NEAR(tr.residual(row), 0,
                            1e-10 * std::max(1., tr.continuityDenominator(row)));
            EXPECT_NEAR(gw.state().continuityResidual(), 0,
                        1e-10 * std::max(1., gw.state().led_init_storage));
            double residual[2]{};
            ASSERT_EQ(swmm_heat_get_ground_values(
                          &source.engine, SWMM_GROUND_HEAT_NORMALIZED_RESIDUAL, 0, 2, residual),
                      SWMM_OK);
            for (double value : residual)
                EXPECT_NEAR(value, 0, 1e-10);
            ASSERT_EQ(source.engine.end(), SWMM_OK);
            ASSERT_EQ(target.engine.end(), SWMM_OK);
            sameMesh(name + "-a", name + "-b", 12);
        }
}
TEST(EngineCheckpoint, MeshGroundReactionMismatchAndCorruptionPreserveFreshTargetsAndOutputs) {
    ThermalSurfaceSession source;
    ASSERT_NO_THROW(startReactiveGroundSession(source, "reactive-refusal-source", 5, 3));
    while (source.engine.context().current_time < 9.1)
        ASSERT_NO_THROW(step(source.engine));
    const auto good = root() / "reactive-refusal.chk", bad = root() / "reactive-corrupt.chk";
    ASSERT_EQ(source.engine.saveCheckpoint(good.string()), "");
    auto corrupt = bytes(good);
    corrupt[corrupt.size() / 2] ^= 1;
    write(bad, corrupt);
    for (int mutation = 0; mutation < 4; ++mutation) {
        const auto name = "reactive-refusal-target-" + std::to_string(mutation);
        ThermalSurfaceSession target;
        ASSERT_NO_THROW(startReactiveGroundSession(target, name, mutation == 2 ? 6 : 5, 3, true,
                                                   mutation == 0 ? ".002" : ".001",
                                                   mutation == 1 ? "UG" : "MG"));
        const auto before = root() / (name + ".chk"), output = root() / (name + ".h5");
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()), "");
        const auto initial = bytes(before), prefix = bytes(output);
        EXPECT_FALSE(target.engine.loadCheckpoint((mutation == 3 ? bad : good).string()).empty());
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()), "");
        EXPECT_EQ(bytes(before), initial);
        EXPECT_EQ(bytes(output), prefix);
        if (mutation == 3) {
            ASSERT_EQ(target.engine.loadCheckpoint(good.string()), "");
            compareMesh(source.engine, target.engine, name + "-retry");
        }
    }
}
#endif

#ifdef OPENSWMM_HAS_2D
namespace {
/**
 * @brief Start a heat-disabled aqueous groundwater clock checkpoint profile.
 *
 * Uses two SI cells, thirty-second CMS run, 0.2 s routing, native deep loss and cell-one source.
 * Writes unchecked deck text, opens then calls startCheckpointSession with no output capability.
 * Rejections throw runtime_error with partial caller engine state retained; no output/report
 * completion or cleanup occurs here.
 *
 * @param[in,out] engine Caller-owned openable engine; managed checkpoint session is started on
 *     success.
 * @param[in] name Raw artifact stem below root for overwritten input/report paths.
 * @param[in] layers Zero selects bulk CLOSED_FORM/default count four; positive selects
 *     SIGMA/LAYERED with that count.
 * @param[in] msx True authors inert bulk A in MG; false authors Dye mg/L plus water age.
 */
void startAqueousClockSession(SWMMEngine& engine, const std::string& name, int layers, bool msx) {
    const auto path = root() / name;
    const std::string species = msx ? "A" : "Dye";
    std::string deck = R"([OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
HEAT_TRANSPORT NO
IGNORE_QUALITY NO
START_DATE 01/01/2026
START_TIME 00:00:00
END_DATE 01/01/2026
END_TIME 00:00:30
ROUTING_STEP .2
REPORT_STEP 00:00:01
[2D_OPTIONS]
REPORT_2D NO
MAX_TIMESTEP .1
LTS_TIERS 1
EVAPORATION NO
TRANSPORT_POLLUTANTS YES
TRANSPORT_MSX YES
TRANSPORT_TEMPERATURE NO
[2D_VERTICES]
0 0 .5
4 0 .5
0 2 .5
4 2 .5
[2D_TRIANGLES]
0 1 2 .03 .04
1 3 2 .03 .03
[2D_AQUIFER_OPTIONS]
NODE_ENROLMENT ROWS
LINK_SEEPAGE NONE
GW_ET NONE
)";
    deck +=
        "CLOSURE " + std::string(layers ? "SIGMA" : "CLOSED_FORM") + "\nM_LAYERS " +
        std::to_string(layers ? layers : 4) +
        "\n[2D_AQUIFER]\n* 36 1 .45 .1 2 HG0 .3 C_LOSS .02\n[GW_TRANSPORT_OPTIONS]\nTHERMAL_MODEL WATER_ONLY\n"
        "CONDUCTION NO\nDISPERSION NO\nUNSAT_TRANSPORT " +
        (layers ? std::string("LAYERED") : std::string("BULK")) + "\n";
    if (msx)
        deck += "[REACTION_OPTIONS]\nSOLVER RK5\nRATE_UNITS SEC\n[REACTION_SPECIES]\nBULK A MG\n";
    else
        deck +=
            "[POLLUTANTS]\nDye MG/L 0 0 0 0 NO * 0 0\n[OPTIONS]\nWATER_AGE ON\n[2D_OPTIONS]\nTRANSPORT_AGE YES\n";
    deck += "[GW_INITIAL_QUALITY]\n* SAT " + species + " 10\n* UNSAT " + species +
            " 10\n[GW_SOURCES]\nW CELL 1 FLOW .000001 " + species + " CONC 3\n";
    write(path.string() + ".inp", deck);
    if (engine.open((path.string() + ".inp").c_str(), (path.string() + ".rpt").c_str(), nullptr) !=
        SWMM_OK)
        throw std::runtime_error(engine.context().error_message);
    if (auto error = engine.startCheckpointSession(SWMM_CHECKPOINT_OUTPUT_NONE, "", false, "");
        !error.empty())
        throw std::runtime_error(error);
}
}
TEST(EngineCheckpoint, AqueousQualitySessionsPreserveAcceptedCellClocksAndPendingContacts) {
    for (int layers : {0, 5})
        for (bool msx : {false, true}) {
            const auto name = "aqueous-clock-" + std::to_string(layers) + "-" + std::to_string(msx);
            SCOPED_TRACE(name);
            SWMMEngine a, b;
            ASSERT_NO_THROW(startAqueousClockSession(a, name + "-a", layers, msx));
            ASSERT_NO_THROW(startAqueousClockSession(b, name + "-b", layers, msx));
            auto& ga = a.surfaceRouter2D().subsurface();
            auto& gb = b.surfaceRouter2D().subsurface();
            EXPECT_FALSE(ga.heat().active());
            EXPECT_EQ(ga.transport().n_msx, msx ? 1 : 0);
            while (a.context().current_time < 4.1)
                ASSERT_NO_THROW(step(a));
            std::vector<double> composition(ga.transport().n_species, 3);
            if (ga.transport().age_row >= 0)
                composition[ga.transport().age_row] = 5;
            const double elevation = ga.state().z_bed[0] + ga.state().zs[0] - .05;
            ga.bookLinkContact(0, .0001, composition, elevation, elevation);
            ASSERT_GT(ga.state().lacc[0], 0);
            ASSERT_GT(ga.cellCompletedTime(0), 0);
            const auto fresh = gb.transportRestartState();
            ASSERT_FALSE(fresh.empty());
            EXPECT_FALSE(gb.restoreTransportRestart({}, true).empty());
            EXPECT_EQ(gb.transportRestartState(), fresh);
            const auto saved = root() / (name + ".chk");
            ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
            ASSERT_EQ(b.loadCheckpoint(saved.string()), "");
            for (int cell = 0; cell < ga.state().n_cells; ++cell)
                EXPECT_DOUBLE_EQ(gb.cellCompletedTime(cell), ga.cellCompletedTime(cell));
            compare(a, b, name + "-restored");
            for (int tick = 0; tick < 15; ++tick) {
                ASSERT_NO_THROW(step(a));
                ASSERT_NO_THROW(step(b));
            }
            compare(a, b, name + "-continued");
            for (int row = 0; row < ga.transport().n_species; ++row)
                EXPECT_NEAR(ga.transport().residual(row), 0, 1e-9);
            EXPECT_NEAR(ga.state().continuityResidual(), 0, 1e-9);
        }
}
#endif

#ifdef OPENSWMM_HAS_2D
namespace {
/**
 * @brief Author and start the managed standalone RK2 surface/GW profile.
 *
 * Builds five-second cross-midnight CMS model with FULL_SWE/order2, 0.2 s routing, 0.05 s mesh max
 * step and CLOSED_FORM aquifer. Mesh output uses FLOAT64 depth/GW rows; binary is requested for
 * BINARY or >=COMBINED mode. Raw writes are unchecked. Open/session errors and callback exceptions
 * propagate with prior mutations retained; caller owns full engine lifetime.
 *
 * @param[in,out] engine Caller-owned openable engine, left in a managed running session on success.
 * @param[in] name Raw evidence stem below root; selected input/report/output files may be
 *     overwritten.
 * @param[in] extra Raw sections appended to the base deck without escaping.
 * @param[in] output Public checkpoint output mode; controls binary destination and HDF5 mesh rows.
 * @param[in] configure Optional synchronous callback after open/before managed start, borrowing
 *     mutable engine; not retained.
 */
void startManagedRk2Session(SWMMEngine& engine, const std::string& name,
                            const std::string& extra = "", int output = SWMM_CHECKPOINT_OUTPUT_NONE,
                            const std::function<void(SWMMEngine&)>& configure = {}) {
    const auto path = root() / name;
    std::string deck = R"([OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
HEAT_TRANSPORT NO
IGNORE_QUALITY YES
IGNORE_2D NO
START_DATE 01/31/2026
START_TIME 23:59:58
END_DATE 02/01/2026
END_TIME 00:00:03
REPORT_START_DATE 01/31/2026
REPORT_START_TIME 23:59:58
ROUTING_STEP .2
REPORT_STEP 00:00:01
[2D_OPTIONS]
MOMENTUM_EQUATION FULL_SWE
RECONSTRUCTION_ORDER 2
REPORT_2D NO
MAX_TIMESTEP .05
LTS_TIERS 1
EVAPORATION NO
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
[2D_VERTICES]
0 0 .5
4 0 .5
0 2 .5
4 2 .5
[2D_TRIANGLES]
0 1 2 .03 .04
1 3 2 .03 .03
[2D_AQUIFER_OPTIONS]
CLOSURE CLOSED_FORM
NODE_ENROLMENT ROWS
LINK_SEEPAGE NONE
GW_ET NONE
[2D_AQUIFER]
* 36 1 .45 .1 2 HG0 .3 C_LOSS .02
)";
    if (output >= SWMM_CHECKPOINT_OUTPUT_MESH)
        deck += "[2D_OPTIONS]\nREPORT_2D YES\nOUTPUT_FILE " + path.string() +
                ".h5\nOUTPUT_PRECISION FLOAT64\nREPORT_2D_VARIABLES DEPTH GROUNDWATER\n";
    deck += extra;
    write(path.string() + ".inp", deck);
    const bool binary =
        output == SWMM_CHECKPOINT_OUTPUT_BINARY || output >= SWMM_CHECKPOINT_OUTPUT_COMBINED;
    if (engine.open((path.string() + ".inp").c_str(), (path.string() + ".rpt").c_str(),
                    binary ? (path.string() + ".out").c_str() : nullptr) != SWMM_OK)
        throw std::runtime_error(engine.context().error_message);
    if (configure)
        configure(engine);
    if (auto error = engine.startCheckpointSession(output, "", false, ""); !error.empty())
        throw std::runtime_error(error);
}
}
namespace {
/**
 * @brief Build a hydraulic network appendix independent of mesh RK2 dynamics.
 *
 * No file IO, parsing or validation occurs; caller appends to an appropriate base deck and supplies
 * safe syntax.
 *
 * @param[in] roughness Raw Manning coefficient token copied into the conduit row.
 * @return Owned storage/outfall/conduit/circular-section text in caller project authoring units.
 */
std::string independentRk2Network(const std::string& roughness = ".013") {
    return "[STORAGE]\nS 0 3 1 FUNCTIONAL 20 1 100 0 0\n[OUTFALLS]\nO 0 FIXED .4 NO\n"
           "[CONDUITS]\nC S O 30 " +
           roughness + " .75 0 0 0\n[XSECTIONS]\nC CIRCULAR .3 0 0 0\n";
}
}
TEST(EngineCheckpoint, ManagedRk2IndependentNetworkRestoresPhysicalRouterAndPendingCalendar) {
    for (bool layered : {false, true}) {
        const auto name = "managed-rk2-independent-" + std::to_string(layered);
        SCOPED_TRACE(name);
        SWMMEngine a, b;
        const auto extra = independentRk2Network() +
                           "[2D_OPTIONS]\nCOUPLING_SYNC .7\n[2D_AQUIFER_OPTIONS]\nCLOSURE " +
                           (layered ? std::string("SIGMA") : std::string("CLOSED_FORM")) +
                           "\nM_LAYERS 5\n";
        ASSERT_NO_THROW(startManagedRk2Session(a, name + "-a", extra));
        ASSERT_NO_THROW(startManagedRk2Session(b, name + "-b", extra));
        const auto initial = a.context().nodes.volume[0];
        double actual = 0;
        ASSERT_EQ(swmm_coupling_advance_to(&a, 1.237, &actual), SWMM_OK);
        ASSERT_DOUBLE_EQ(actual, 1.237);
        EXPECT_GT(std::abs(a.context().links.flow[0]), 0);
        EXPECT_NE(a.context().nodes.volume[0], initial);
        const auto saved = root() / (name + ".chk");
        ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
        ASSERT_EQ(b.loadCheckpoint(saved.string()), "");
        compare(a, b, name + "-restored");
        while (a.context().current_time < 4.7) {
            ASSERT_NO_THROW(step(a));
            ASSERT_NO_THROW(step(b));
            compare(a, b, name + "-continued");
        }
        EXPECT_NEAR(a.surfaceRouter2D().subsurface().state().continuityResidual(), 0, 1e-10);
        EXPECT_EQ(a.context().mass_balance_2d.coupling_1d_to_2d_in, 0);
        EXPECT_EQ(a.context().mass_balance_2d.coupling_2d_to_1d_out, 0);
    }
}
TEST(EngineCheckpoint, ManagedRk2IndependentNetworkDoesNotChangeFixedCadenceMeshTrajectory) {
    SWMMEngine a, b;
    ASSERT_NO_THROW(
        startManagedRk2Session(a, "managed-rk2-independent-mesh", independentRk2Network()));
    ASSERT_NO_THROW(startManagedRk2Session(b, "managed-rk2-independent-reference"));
    while (a.context().current_time < 4.7) {
        ASSERT_NO_THROW(step(a));
        ASSERT_NO_THROW(step(b));
        ASSERT_EQ(a.context().current_time, b.context().current_time);
        EXPECT_EQ(a.surfaceRouter2D().state().volume, b.surfaceRouter2D().state().volume);
        EXPECT_EQ(a.surfaceRouter2D().subsurface().transportRestartState(),
                  b.surfaceRouter2D().subsurface().transportRestartState());
    }
}
TEST(EngineCheckpoint, ManagedRk2IndependentMeshDoesNotChangeFixedCadenceNetworkTrajectory) {
    SWMMEngine a, b;
    ASSERT_NO_THROW(
        startManagedRk2Session(a, "managed-rk2-independent-network", independentRk2Network()));
    ASSERT_NO_THROW(startManagedRk2Session(b, "managed-rk2-independent-network-reference",
                                           independentRk2Network() + "[OPTIONS]\nIGNORE_2D YES\n"));
    while (a.context().current_time < 4.7) {
        ASSERT_NO_THROW(step(a));
        ASSERT_NO_THROW(step(b));
        ASSERT_EQ(a.context().current_time, b.context().current_time);
        EXPECT_EQ(a.context().nodes.volume, b.context().nodes.volume);
        EXPECT_EQ(a.context().nodes.depth, b.context().nodes.depth);
        EXPECT_EQ(a.context().links.flow, b.context().links.flow);
        EXPECT_DOUBLE_EQ(a.context().mass_balance.routing_final_storage,
                         b.context().mass_balance.routing_final_storage);
        EXPECT_DOUBLE_EQ(a.context().mass_balance.routing_outflow,
                         b.context().mass_balance.routing_outflow);
    }
}
TEST(EngineCheckpoint, ManagedRk2IndependentNetworkMismatchPreservesTarget) {
    SWMMEngine a, b;
    ASSERT_NO_THROW(
        startManagedRk2Session(a, "managed-rk2-independent-model-source", independentRk2Network()));
    ASSERT_NO_THROW(startManagedRk2Session(b, "managed-rk2-independent-model-target",
                                           independentRk2Network(".014")));
    ASSERT_NO_THROW(step(a));
    const auto good = root() / "managed-rk2-independent-model.chk",
               before = root() / "managed-rk2-independent-before.chk";
    ASSERT_EQ(a.saveCheckpoint(good.string()), "");
    ASSERT_EQ(b.saveCheckpoint(before.string()), "");
    const auto initial = bytes(before);
    EXPECT_FALSE(b.loadCheckpoint(good.string()).empty());
    ASSERT_EQ(b.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(before), initial);
}
TEST(EngineCheckpoint,
     ManagedRk2IndependentNetworkStillRefusesSharedExchangeAndUnsupportedQuality) {
    for (
        const auto& extra :
        {std::string("[2D_TRIANGLE_NODE_MAP]\n0 S 0 1\n"),
         std::string(
             "[OPTIONS]\nIGNORE_QUALITY NO\nQUALITY_SOLVER EULERIAN_ARD\n[POLLUTANTS]\nDye MG/L 0 0 0 0 NO * 0 0\n")}) {
        SWMMEngine engine;
        const auto name = "managed-rk2-independent-refusal-" + std::to_string(extra.size());
        EXPECT_THROW(startManagedRk2Session(engine, name, independentRk2Network() + extra),
                     std::runtime_error);
    }
}
TEST(EngineCheckpoint, ManagedRk2WaterSessionRestoresActualNumericalOwnerAndCalendar) {
    SWMMEngine a, b;

    ASSERT_NO_THROW(startManagedRk2Session(a, "managed-rk2-water-a"));

    ASSERT_NO_THROW(startManagedRk2Session(b, "managed-rk2-water-b"));
    ASSERT_EQ(a.surfaceRouter2D().options().reconstruction_order, 2);
    ASSERT_FALSE(a.surfaceRouter2D().commonSourceAttached());
    while (a.context().current_time < 2.3)
        ASSERT_NO_THROW(step(a));
    const auto saved = root() / "managed-rk2-water.chk";
    ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.loadCheckpoint(saved.string()), "");
    compare(a, b, "managed-rk2-water-restored");
    while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(a));

        ASSERT_NO_THROW(step(b));
        compare(a, b, "managed-rk2-water-continued");
    }
    EXPECT_NEAR(a.surfaceRouter2D().subsurface().state().continuityResidual(), 0, 1e-10);
}
#endif

#ifdef OPENSWMM_HAS_2D
namespace {
/**
 * @brief Poison a managed RK2 cell volume while recomputing nested envelope checksums.
 *
 * Requires exactly two volume entries and a bounded fixed native-field offset. Throws on missing
 * markers/layout/count/bounds errors. Recomputes CPU CRC and router envelope, then replaces its
 * nested blob in the original; does not validate/update unrelated outer contracts or write files.
 *
 * @param[in] data Owned-by-value known-layout full checkpoint bytes containing managed-router and
 *     CPU records.
 * @return Owned re-encoded checkpoint with first stored cell volume replaced by quiet NaN.
 */
std::string invalidManagedRk2Volume(std::string data) {
    const std::string router_magic = "OpenSWMM managed standalone RK2 router boundary",
                      cpu_magic = "OpenSWMM CPU surface/source boundary";
    auto router = nested(data, router_magic).second, router_payload = surfacePayload(router),
         cpu = nested(router_payload, cpu_magic).second;
    detail::CompletedSourceArchive in(cpu);
    std::string cm, cp;
    uint32_t cv = 0, ce = 0, cl = 0, cc = 0;
    in(cm, cv, ce, cl, cp, cc);
    in.finish();
    detail::CompletedSourceArchive body(cp);
    std::string numerical_model;
    std::vector<double> depth, head, volume;
    body(numerical_model, depth, head, volume);
    if (volume.size() != 2)
        throw std::runtime_error("Unexpected managed RK2 fixture cell count");
    const auto location = sizeof(uint64_t) + numerical_model.size() +
                          2 * (sizeof(uint64_t) + 2 * sizeof(double)) + sizeof(uint64_t);
    if (location + sizeof(double) > cp.size())
        throw std::runtime_error("Missing managed RK2 fixture volume");
    double nan = std::numeric_limits<double>::quiet_NaN();
    std::memcpy(cp.data() + location, &nan, sizeof(nan));
    cc = crc(cp);
    detail::CompletedSourceArchive changed;
    changed(cm, cv, ce, cl, cp, cc);
    router_payload = replaceNested(router_payload, cpu_magic, changed.bytes());
    router = withSurfacePayload(router, router_payload);
    data = replaceNested(data, router_magic, router);
    return data;
}

/**
 * @brief Build optional aqueous/age/reaction/thermal appendices for the managed RK2 fixture.
 *
 * Raw closure is copied unescaped and flag compatibility is left to downstream validation. Uses RK5
 * SEC rates, fixed species concentrations/native age seeds and adiabatic thermal boundaries; no
 * file IO or simulation occurs.
 *
 * @param[in] closure Raw groundwater closure keyword.
 * @param[in] layered Select LAYERED rather than BULK unsaturated transport.
 * @param[in] temperature Enable temperature rows and transport with fixed -4 C surface/-2 C GW
 *     seeds.
 * @param[in] porous Select POROUS_EQUILIBRIUM with conduction rather than WATER_ONLY without
 *     conduction.
 * @param[in] reactions Enable Dye/age/MSX A-to-B reactions, sorption and a groundwater source.
 * @return Owned section text using four aqueous and three thermal layers.
 */
std::string managedRk2Quality(const std::string& closure, bool layered, bool temperature,
                              bool porous, bool reactions) {
    std::string text =
        "\n[2D_AQUIFER_OPTIONS]\nCLOSURE " + closure +
        "\nM_LAYERS 4\n[GW_TRANSPORT_OPTIONS]\nUNSAT_TRANSPORT " +
        (layered ? std::string("LAYERED") : std::string("BULK")) + "\nTHERMAL_MODEL " +
        (porous ? std::string("POROUS_EQUILIBRIUM") : std::string("WATER_ONLY")) +
        "\nTHERMAL_LAYERS 3\nCONDUCTION " + (porous ? std::string("YES") : std::string("NO")) +
        "\nDISPERSION NO\nSURFACE_THERMAL_BC ADIABATIC\nDEEP_THERMAL_BC ADIABATIC\n";
    if (temperature)
        text +=
            "[OPTIONS]\nHEAT_TRANSPORT YES\nWATER_DENSITY 1000\nWATER_SPECIFIC_HEAT_CAPACITY 4186\n[2D_OPTIONS]\nHEAT_MODEL TRANSPORT_ONLY\nTRANSPORT_TEMPERATURE YES\n[GW_TRANSPORT_OPTIONS]\nTRANSPORT_TEMPERATURE YES\n[2D_INITIAL_QUALITY]\n* __TEMPERATURE__ -4\n[GW_INITIAL_QUALITY]\n* SAT __TEMPERATURE__ -2\n* UNSAT __TEMPERATURE__ -2\n";
    if (reactions)
        text +=
            "[OPTIONS]\nIGNORE_QUALITY NO\nWATER_AGE ON\n[POLLUTANTS]\nDye MG/L 0 0 0 0 NO * 0 0\n[2D_OPTIONS]\nTRANSPORT_POLLUTANTS YES\nTRANSPORT_MSX YES\nTRANSPORT_AGE YES\n[GW_TRANSPORT_OPTIONS]\nTRANSPORT_POLLUTANTS YES\nTRANSPORT_MSX YES\nTRANSPORT_AGE YES\n[REACTION_OPTIONS]\nSOLVER RK5\nRATE_UNITS SEC\nCOUPLING FULL\nATOL 1e-13\nRTOL 1e-11\n[REACTION_SPECIES]\nBULK A MG\nBULK B MG\n[REACTION_PIPES]\nRATE A -.02*A\nRATE B .02*A\n[REACTION_SUBSURFACE]\n* SAT A RATE -.01*A\n* SAT B RATE .01*A\n* UNSAT A RATE -.01*A\n* UNSAT B RATE .01*A\n[GW_SORPTION]\n* A 2 0\n* B 2 0\n[2D_INITIAL_QUALITY]\n* Dye 3\n* A 2\n* __WATER_AGE__ .001\n[GW_INITIAL_QUALITY]\n* SAT Dye 4\n* UNSAT Dye 5\n* SAT A 7\n* UNSAT A 11\n* SAT __WATER_AGE__ 2\n* UNSAT __WATER_AGE__ 3\n[GW_SOURCES]\nW * FLOW .000001 Dye CONC 3 A CONC 1 B CONC 0 __WATER_AGE__ CONC 2\n";
    return text;
}
/**
 * @brief Compare managed RK2 physical checkpoints with optional mesh-output stripping.
 *
 * Delegates to compare or compareMesh without stepping. Does not compare result dataset contents;
 * save/assertion/decode errors follow the delegated helper.
 *
 * @param[in,out] a Reference checkpointable engine.
 * @param[in,out] b Candidate engine at the expected same boundary.
 * @param[in] name Raw evidence stem used by the delegated save/compare helper.
 * @param[in] mesh Use compareMesh when compiled mesh-output test support is available; otherwise
 *     this flag is ignored.
 */
void compareManagedRk2(SWMMEngine& a, SWMMEngine& b, const std::string& name, bool mesh) {
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
    if (mesh) {
        compareMesh(a, b, name);
        return;
    }
#else
    (void)mesh;
#endif
    compare(a, b, name);
}
/**
 * @brief Restore a second engine at 2.3 seconds and compare every later accepted boundary.
 *
 * Steps a until current_time>=2.3, saves, loads b, compares restored then steps both until a total
 * duration. Requires progress/reachable clocks, with no iteration guard; last threshold step may
 * overshoot. Save/load/step exceptions and assertion failures retain partial state; caller
 * ends/closes both engines afterward.
 *
 * @param[in,out] a Reference managed engine that can advance through 2.3 s to total duration.
 * @param[in,out] b Fresh compatible managed target engine whose state is replaced by restore.
 * @param[in] name Raw checkpoint/equality evidence stem below root.
 * @param[in] mesh Select mesh-output physical comparison through compareManagedRk2.
 */
void continueManagedRk2Pair(SWMMEngine& a, SWMMEngine& b, const std::string& name,
                            bool mesh = false) {
    while (a.context().current_time < 2.3)
        step(a);
    const auto saved = root() / (name + ".chk");
    if (auto error = a.saveCheckpoint(saved.string()); !error.empty())
        throw std::runtime_error(error);
    if (auto error = b.loadCheckpoint(saved.string()); !error.empty())
        throw std::runtime_error(error);
    compareManagedRk2(a, b, name + "-restored", mesh);
    while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
        step(a);
        step(b);
        compareManagedRk2(a, b, name + "-continued", mesh);
    }
}
}
TEST(EngineCheckpoint, ManagedRk2AqueousAgeAndReactionsContinueAcrossMidnight) {
    for (const auto& closure : {"CLOSED_FORM", "ENSLAVED", "SIGMA"})
        for (bool layered : {false, true}) {
            if (layered && std::string(closure) != "SIGMA")
                continue;
            const auto name =
                "managed-rk2-reactive-" + std::string(closure) + std::to_string(layered);
            SCOPED_TRACE(name);
            SWMMEngine a, b;
            const auto extra = managedRk2Quality(closure, layered, false, false, true);

            ASSERT_NO_THROW(startManagedRk2Session(a, name + "-a", extra));

            ASSERT_NO_THROW(startManagedRk2Session(b, name + "-b", extra));

            ASSERT_NO_THROW(continueManagedRk2Pair(a, b, name));
            const auto& tr = a.surfaceRouter2D().subsurface().transport();
            for (int row = 0; row < tr.n_species; ++row)
                EXPECT_NEAR(tr.residual(row), 0, 1e-10 * std::max(1., tr.init_mass[row]));
            EXPECT_GT(tr.generated_age[tr.age_row], 0);
            EXPECT_GT(a.context().current_date,
                      datetime::addSeconds(a.context().options.start_date, 2));
        }
}
TEST(EngineCheckpoint, ManagedRk2WaterAndPorousTemperatureOwnersContinueExactly) {
    for (const auto& closure : {"CLOSED_FORM", "ENSLAVED", "SIGMA"})
        for (bool layered : {false, true})
            for (bool porous : {false, true}) {
                if (layered && std::string(closure) != "SIGMA")
                    continue;
                const auto name = "managed-rk2-thermal-" + std::string(closure) +
                                  std::to_string(layered) + std::to_string(porous);
                SCOPED_TRACE(name);
                SWMMEngine a, b;
                const auto extra = managedRk2Quality(closure, layered, true, porous, false);

                ASSERT_NO_THROW(startManagedRk2Session(a, name + "-a", extra));

                ASSERT_NO_THROW(startManagedRk2Session(b, name + "-b", extra));

                ASSERT_NO_THROW(continueManagedRk2Pair(a, b, name));
                EXPECT_EQ(a.surfaceRouter2D().subsurface().heat().active(), porous);
            }
}
TEST(EngineCheckpoint, ManagedRk2CopiedProvidersAndExpiryAreOwnedAcrossContinuation) {
    for (bool porous : {false, true}) {
        const auto name = "managed-rk2-providers-" + std::to_string(porous);
        SWMMEngine a, b;
        const auto extra = managedRk2Quality("SIGMA", true, true, porous, false);

        ASSERT_NO_THROW(startManagedRk2Session(a, name + "-a", extra));

        ASSERT_NO_THROW(startManagedRk2Session(b, name + "-b", extra));
        unsigned flags = 0;
        ASSERT_EQ(swmm_coupling_capabilities(&a, &flags), SWMM_OK);
        ASSERT_EQ(flags & (1 | 2 | 4 | 16 | 32 | 64 | 128 | 256 | 512),
                  unsigned(1 | 2 | 4 | 16 | 32 | 64 | 128 | 256 | 512));
        double concentration = -8, rate = 0;
        SWMM_CouplingSource source[2]{{sizeof(SWMM_CouplingSource), "surface", 0, 0, .001, 0, 1.735,
                                       1, &concentration, &rate},
                                      {sizeof(SWMM_CouplingSource), "well", 1, 1, .001, 4186, 1.237,
                                       1, &concentration, &rate}};
        SWMM_CouplingForcing forcing{sizeof(forcing), 1, -1, 1, .0001, 1.537, 1, &concentration};
        int edge = -1;
        const auto& mesh = a.surfaceRouter2D().mesh();
        for (int k = 0; k < mesh.cell_vertex_count(0); ++k)
            if (mesh.cell_neighbour(0, k) < 0) {
                edge = k;
                break;
            }
        ASSERT_GE(edge, 0);
        SWMM_CouplingBoundary boundary{sizeof(boundary), 1, 0, edge, 2, -.0001, 1, &concentration};
        SWMM_CouplingFrame frame{sizeof(frame), 1, "managed", 2, source, 1,
                                 &boundary,     1, &forcing,  0, nullptr};
        ASSERT_EQ(swmm_coupling_apply_frame(&a, &frame), SWMM_OK);
        double actual = 0;
        ASSERT_EQ(swmm_coupling_advance_to(&a, 1, &actual), SWMM_OK);
        ASSERT_EQ(actual, 1);
        const auto saved = root() / (name + ".chk");
        ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
        ASSERT_EQ(b.loadCheckpoint(saved.string()), "");
        compare(a, b, name + "-restored");
        while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a));

            ASSERT_NO_THROW(step(b));
            compare(a, b, name + "-continued");
        }
        for (const auto& row : a.surfaceRouter2D().state().runtime_sources.rows)
            EXPECT_FALSE(row.enabled);
        for (const auto& row : a.surfaceRouter2D().subsurface().runtimeSources().rows)
            if (row.boundary_edge < 0)
                EXPECT_FALSE(row.enabled);
    }
}
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
namespace {
/**
 * @brief Recursively compare HDF5 group link schemas, dataset types/shapes and values.
 *
 * Enumerates x links, requires equal link counts/matching names and group/dataset kinds, then
 * compares dataset datatype/dataspace. Variable strings compare by text and reclaim allocations;
 * other non-VLEN datasets compare bytes read with matching file datatypes. Does not compare
 * attributes, external-link identity or creation properties. Successful path closes temporary IDs;
 * fatal assertions can bypass raw-ID cleanup. Requires nonoverflowing dataset byte sizes and no
 * concurrent mutation; recursion has no cycle guard.
 *
 * @param[in] x Borrowed open reference file/group identifier.
 * @param[in] y Borrowed open comparison identifier; both trees must be stable and acyclic.
 */
void sameManagedRk2HdfGroup(hid_t x, hid_t y) {
    H5G_info_t gx{}, gy{};
    ASSERT_GE(H5Gget_info(x, &gx), 0);
    ASSERT_GE(H5Gget_info(y, &gy), 0);
    ASSERT_EQ(gx.nlinks, gy.nlinks);
    for (hsize_t k = 0; k < gx.nlinks; ++k) {
        const auto length =
            H5Lget_name_by_idx(x, ".", H5_INDEX_NAME, H5_ITER_INC, k, nullptr, 0, H5P_DEFAULT);
        ASSERT_GE(length, 0);
        std::string name(std::size_t(length) + 1, '\0');
        ASSERT_GE(H5Lget_name_by_idx(x, ".", H5_INDEX_NAME, H5_ITER_INC, k, name.data(),
                                     name.size(), H5P_DEFAULT),
                  0);
        name.resize(std::size_t(length));
        SCOPED_TRACE(name);
        ASSERT_GT(H5Lexists(y, name.c_str(), H5P_DEFAULT), 0);
        H5O_info_t ox{}, oy{};
        ASSERT_GE(H5Oget_info_by_name(x, name.c_str(), &ox, H5O_INFO_BASIC, H5P_DEFAULT), 0);
        ASSERT_GE(H5Oget_info_by_name(y, name.c_str(), &oy, H5O_INFO_BASIC, H5P_DEFAULT), 0);
        ASSERT_EQ(ox.type, oy.type);
        if (ox.type == H5O_TYPE_GROUP) {
            hid_t a = H5Gopen2(x, name.c_str(), H5P_DEFAULT),
                  b = H5Gopen2(y, name.c_str(), H5P_DEFAULT);
            ASSERT_GE(a, 0);
            ASSERT_GE(b, 0);
            sameManagedRk2HdfGroup(a, b);
            H5Gclose(a);
            H5Gclose(b);
            continue;
        }
        ASSERT_EQ(ox.type, H5O_TYPE_DATASET);
        hid_t a = H5Dopen2(x, name.c_str(), H5P_DEFAULT),
              b = H5Dopen2(y, name.c_str(), H5P_DEFAULT);
        ASSERT_GE(a, 0);
        ASSERT_GE(b, 0);
        hid_t at = H5Dget_type(a), bt = H5Dget_type(b), as = H5Dget_space(a), bs = H5Dget_space(b);
        ASSERT_GT(H5Tequal(at, bt), 0);
        ASSERT_GT(H5Sextent_equal(as, bs), 0);
        const auto count = H5Sget_simple_extent_npoints(as);
        ASSERT_GE(count, 0);
        if (H5Tis_variable_str(at) > 0) {
            std::vector<char*> av(std::size_t(count), nullptr), bv(std::size_t(count), nullptr);
            ASSERT_GE(H5Dread(a, at, H5S_ALL, H5S_ALL, H5P_DEFAULT, av.data()), 0);
            ASSERT_GE(H5Dread(b, bt, H5S_ALL, H5S_ALL, H5P_DEFAULT, bv.data()), 0);
            for (std::size_t i = 0; i < av.size(); ++i)
                EXPECT_STREQ(av[i], bv[i]);
            H5Dvlen_reclaim(at, as, H5P_DEFAULT, av.data());
            H5Dvlen_reclaim(bt, bs, H5P_DEFAULT, bv.data());
        } else {
            ASSERT_NE(H5Tget_class(at), H5T_VLEN);
            std::vector<unsigned char> av(std::size_t(count) * H5Tget_size(at)), bv(av.size());
            ASSERT_GE(H5Dread(a, at, H5S_ALL, H5S_ALL, H5P_DEFAULT, av.data()), 0);
            ASSERT_GE(H5Dread(b, bt, H5S_ALL, H5S_ALL, H5P_DEFAULT, bv.data()), 0);
            EXPECT_EQ(av, bv);
        }
        H5Sclose(as);
        H5Sclose(bs);
        H5Tclose(at);
        H5Tclose(bt);
        H5Dclose(a);
        H5Dclose(b);
    }
}
}
TEST(EngineCheckpoint, ManagedRk2NativeMeshHistoryPreservesReportCalendarAcrossMidnight) {
    for (bool thermal : {false, true}) {
        const auto name = "managed-rk2-mesh-" + std::to_string(thermal);
        SWMMEngine a, b;
        auto extra = managedRk2Quality("SIGMA", true, thermal, thermal, !thermal);
        if (thermal)
            extra += "[2D_OPTIONS]\nREPORT_2D_VARIABLES DEPTH HEAT GROUNDWATER\n";

        ASSERT_NO_THROW(startManagedRk2Session(a, name + "-a", extra, SWMM_CHECKPOINT_OUTPUT_MESH));

        ASSERT_NO_THROW(startManagedRk2Session(b, name + "-b", extra, SWMM_CHECKPOINT_OUTPUT_MESH));

        ASSERT_NO_THROW(continueManagedRk2Pair(a, b, name, true));
        ASSERT_EQ(a.end(), SWMM_OK);
        ASSERT_EQ(b.end(), SWMM_OK);
        hid_t x = H5Fopen((root() / (name + "-a.h5")).string().c_str(), H5F_ACC_RDONLY,
                          H5P_DEFAULT),
              y = H5Fopen((root() / (name + "-b.h5")).string().c_str(), H5F_ACC_RDONLY,
                          H5P_DEFAULT);
        ASSERT_GE(x, 0);
        ASSERT_GE(y, 0);
        sameManagedRk2HdfGroup(x, y);
        H5Fclose(x);
        H5Fclose(y);
    }
}
#endif
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint,
     ManagedRk2PreparedWeatherRestoresAirAndSharedRadiationWithoutChangingTheFreshModel) {
    for (bool atmosphere : {false, true}) {
        const auto name = "managed-rk2-weather-" + std::to_string(atmosphere);
        SWMMEngine a, b;
        auto extra =
            managedRk2Quality("SIGMA", true, true, true, false) +
            "[TEMPERATURE]\nTIMESERIES AirTemp\nHUMIDITY 50\nWINDSPEED MONTHLY 5 5 5 5 5 5 5 5 5 5 5 5\n[TIMESERIES]\nAirTemp 01/31/2026 23:59:58 5\nAirTemp 02/01/2026 00:00:00 35\nAirTemp 02/01/2026 00:00:03 15\n";
        if (atmosphere)
            extra +=
                "[2D_OPTIONS]\nHEAT_MODEL COUPLED\nBED_HEAT_MODEL GROUND_COLUMN\nHEAT_SURFACE_EXCHANGE YES\nHEAT_RADIATIVE_EXCHANGE YES\n[GW_TRANSPORT_OPTIONS]\nSURFACE_THERMAL_BC SURFACE_WATER\n";
        else
            extra += "[GW_TRANSPORT_OPTIONS]\nSURFACE_THERMAL_BC AIR\n";
        const auto configure = [&](SWMMEngine& e) {
            if (!atmosphere)
                return;
            auto& config = e.context().snowpacks.config;
            config.site = surface::ClimateSite{45, -120, -8, 1000};
            config.monthly_range.fill(10.);
            config.shortwave.authored = config.longwave.authored = true;
        };

        ASSERT_NO_THROW(
            startManagedRk2Session(a, name + "-a", extra, SWMM_CHECKPOINT_OUTPUT_NONE, configure));

        ASSERT_NO_THROW(
            startManagedRk2Session(b, name + "-b", extra, SWMM_CHECKPOINT_OUTPUT_NONE, configure));
        const double old_air = b.context().climate_state.temperature;
        while (a.context().current_time < 2.3)
            ASSERT_NO_THROW(step(a));
        EXPECT_NE(a.context().climate_state.temperature, old_air);
        if (atmosphere) {
            EXPECT_TRUE(a.context().climate_state.radiation.shared_heat_forcing);
            EXPECT_GT(a.context().climate_state.radiation.longwave, 0);
        }
        const auto saved = root() / (name + ".chk");
        ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
        ASSERT_EQ(b.loadCheckpoint(saved.string()), "");
        EXPECT_EQ(a.context().climate_state.temperature, b.context().climate_state.temperature);
        EXPECT_EQ(a.context().climate_state.radiation.longwave,
                  b.context().climate_state.radiation.longwave);
        compare(a, b, name + "-restored");
        while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a));

            ASSERT_NO_THROW(step(b));
            compare(a, b, name + "-continued");
        }
    }
}
TEST(EngineCheckpoint, ManagedRk2CorruptClockAndNumericalStateRefuseAtomicallyWithValidRetry) {
    SWMMEngine source;
    const auto extra = managedRk2Quality("SIGMA", true, true, true, false);

    ASSERT_NO_THROW(startManagedRk2Session(source, "managed-rk2-corrupt-source", extra));
    while (source.context().current_time < 1.3)
        ASSERT_NO_THROW(step(source));
    const auto good = root() / "managed-rk2-corrupt-good.chk";
    ASSERT_EQ(source.saveCheckpoint(good.string()), "");
    detail::CompletedSourceArchive input(bytes(good));
    std::string magic, model, payload;
    uint32_t version, sum;
    input(magic, version, model, sum, payload);
    input.finish();
    for (int kind : {0, 1, 2, 3}) {
        const auto name = "managed-rk2-corrupt-" + std::to_string(kind);
        SWMMEngine target;

        ASSERT_NO_THROW(startManagedRk2Session(target, name, extra));
        auto data = payload;
        if (kind == 0) {
            double nan = std::numeric_limits<double>::quiet_NaN();
            std::memcpy(data.data(), &nan, sizeof(nan));
        }
        if (kind == 1) {
            double mismatch = 1;
            std::memcpy(data.data() + sizeof(double), &mismatch, sizeof(mismatch));
        }
        if (kind == 2)
            data = invalidManagedRk2Volume(data);
        if (kind == 3)
            data += "extra";
        sum = crc(data);
        detail::CompletedSourceArchive out;
        out(magic, version, model, sum, data);
        const auto bad = root() / (name + "-bad.chk"), before = root() / (name + "-before.chk"),
                   after = root() / (name + "-after.chk");
        write(bad, out.bytes());
        ASSERT_EQ(target.saveCheckpoint(before.string()), "");
        EXPECT_FALSE(target.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(target.saveCheckpoint(after.string()), "");
        EXPECT_EQ(bytes(before), bytes(after));
        ASSERT_EQ(target.loadCheckpoint(good.string()), "");
        compare(source, target, name + "-retry");
    }
}
TEST(EngineCheckpoint, ManagedRk2ChangedProjectAndExistingRuntimeOwnerAreNotReboundByRestore) {
    SWMMEngine source, changed, active;
    const auto extra = managedRk2Quality("SIGMA", true, true, true, false);

    ASSERT_NO_THROW(startManagedRk2Session(source, "managed-rk2-model-source", extra));

    ASSERT_NO_THROW(startManagedRk2Session(changed, "managed-rk2-model-changed",
                                           extra + "[GW_TRANSPORT_OPTIONS]\nTHERMAL_LAYERS 4\n"));

    ASSERT_NO_THROW(startManagedRk2Session(active, "managed-rk2-model-active", extra));
    while (source.context().current_time < 1.3)
        ASSERT_NO_THROW(step(source));
    const auto good = root() / "managed-rk2-model-good.chk";
    ASSERT_EQ(source.saveCheckpoint(good.string()), "");
    double concentration = -8, rate = 0;
    ASSERT_EQ(swmm_coupling_set_source(&active, "already-owned", 0, 0, .001, 0, &concentration,
                                       &rate, 1, 0),
              SWMM_OK);
    for (auto* target : {&changed, &active}) {
        const auto before = root() / (target == &changed ? "managed-rk2-changed-before.chk"
                                                         : "managed-rk2-active-before.chk"),
                   after = root() / (target == &changed ? "managed-rk2-changed-after.chk"
                                                        : "managed-rk2-active-after.chk");
        ASSERT_EQ(target->saveCheckpoint(before.string()), "");
        EXPECT_FALSE(target->loadCheckpoint(good.string()).empty());
        ASSERT_EQ(target->saveCheckpoint(after.string()), "");
        EXPECT_EQ(bytes(before), bytes(after));
    }
}
#endif

#if defined(OPENSWMM_HAS_2D) && defined(OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST)
TEST(EngineCheckpoint, ManagedRk2InvalidNumericalOwnerCannotPublishAValidMeshPrefix) {
    SWMMEngine source, target;
    const auto extra = managedRk2Quality("SIGMA", true, true, true, false);
    ASSERT_NO_THROW(startManagedRk2Session(source, "managed-rk2-output-atomic-source", extra,
                                           SWMM_CHECKPOINT_OUTPUT_MESH));
    ASSERT_NO_THROW(startManagedRk2Session(target, "managed-rk2-output-atomic-target", extra,
                                           SWMM_CHECKPOINT_OUTPUT_MESH));
    while (source.context().current_time < 2.3)
        ASSERT_NO_THROW(step(source));
    const auto good = root() / "managed-rk2-output-atomic-good.chk",
               bad = root() / "managed-rk2-output-atomic-bad.chk",
               before = root() / "managed-rk2-output-atomic-before.chk",
               after = root() / "managed-rk2-output-atomic-after.chk";
    ASSERT_EQ(source.saveCheckpoint(good.string()), "");
    detail::CompletedSourceArchive in(bytes(good));
    std::string magic, model, payload;
    uint32_t version = 0, sum = 0;
    in(magic, version, model, sum, payload);
    in.finish();
    payload = invalidManagedRk2Volume(payload);
    sum = crc(payload);
    detail::CompletedSourceArchive out;
    out(magic, version, model, sum, payload);
    write(bad, out.bytes());
    ASSERT_EQ(target.saveCheckpoint(before.string()), "");
    const auto destination = root() / "managed-rk2-output-atomic-target.h5";
    const auto prior = bytes(destination);
    EXPECT_FALSE(target.loadCheckpoint(bad.string()).empty());
    ASSERT_EQ(target.saveCheckpoint(after.string()), "");
    EXPECT_TRUE(bytes(before) == bytes(after));
    EXPECT_TRUE(bytes(destination) == prior);
    ASSERT_EQ(target.loadCheckpoint(good.string()), "");
    compareMesh(source, target, "managed-rk2-output-atomic-retry");
    while (source.context().elapsed_ms < source.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(source));
        ASSERT_NO_THROW(step(target));
        compareMesh(source, target, "managed-rk2-output-atomic-continued");
    }
}
#endif
#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, ManagedRk2CorruptCopiedPrescriptionRefusesBeforeReplayAndAllowsRetry) {
    SWMMEngine source, target;
    ASSERT_NO_THROW(startManagedRk2Session(source, "managed-rk2-replay-source"));
    ASSERT_NO_THROW(startManagedRk2Session(target, "managed-rk2-replay-target"));
    SWMM_CouplingSource supply{sizeof(supply), "supply", 0, 0, .001, 0, 0, 0, nullptr, nullptr};
    SWMM_CouplingFrame frame{sizeof(frame), 1, "replay", 1, &supply, 0,
                             nullptr,       0, nullptr,  0, nullptr};
    ASSERT_EQ(swmm_coupling_apply_frame(&source, &frame), SWMM_OK);
    while (source.context().current_time < 1.3)
        ASSERT_NO_THROW(step(source));
    const auto good = root() / "managed-rk2-replay-good.chk",
               bad = root() / "managed-rk2-replay-bad.chk",
               before = root() / "managed-rk2-replay-before.chk",
               after = root() / "managed-rk2-replay-after.chk";
    ASSERT_EQ(source.saveCheckpoint(good.string()), "");
    detail::CompletedSourceArchive in(bytes(good));
    std::string magic, model, payload;
    uint32_t version = 0, sum = 0;
    in(magic, version, model, sum, payload);
    in.finish();
    const std::string router_magic = "OpenSWMM managed standalone RK2 router boundary";
    auto router = nested(payload, router_magic).second, body = surfacePayload(router);
    const auto marker = body.find("managed RK2 copied prescriptions v1");
    ASSERT_NE(marker, std::string::npos);
    const auto id = body.find("provider/replay/supply", marker);
    ASSERT_NE(id, std::string::npos);
    body[id] = 'x';
    router = withSurfacePayload(router, body);
    payload = replaceNested(payload, router_magic, router);
    sum = crc(payload);
    detail::CompletedSourceArchive out;
    out(magic, version, model, sum, payload);
    write(bad, out.bytes());
    ASSERT_EQ(target.saveCheckpoint(before.string()), "");
    EXPECT_FALSE(target.loadCheckpoint(bad.string()).empty());
    ASSERT_EQ(target.saveCheckpoint(after.string()), "");
    EXPECT_TRUE(bytes(before) == bytes(after));
    ASSERT_EQ(target.loadCheckpoint(good.string()), "");
    compare(source, target, "managed-rk2-replay-retry");
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, ManagedRk2MalformedForcingDescriptorsRefuseBeforeIndexedReplay) {
    SWMMEngine source;
    ASSERT_NO_THROW(startManagedRk2Session(source, "managed-rk2-forcing-source"));
    SWMM_CouplingForcing rain{sizeof(rain), 1, 0, 1, .001, 5, 0, nullptr};
    SWMM_CouplingFrame frame{sizeof(frame), 1, "weather", 0, nullptr, 0,
                             nullptr,       1, &rain,     0, nullptr};
    ASSERT_EQ(swmm_coupling_apply_frame(&source, &frame), SWMM_OK);
    while (source.context().current_time < 1.3)
        ASSERT_NO_THROW(step(source));
    const auto good = root() / "managed-rk2-forcing-good.chk";
    ASSERT_EQ(source.saveCheckpoint(good.string()), "");
    detail::CompletedSourceArchive input(bytes(good));
    std::string magic, model, payload;
    uint32_t version = 0, sum = 0;
    input(magic, version, model, sum, payload);
    input.finish();
    const std::string router_magic = "OpenSWMM managed standalone RK2 router boundary",
                      record = "managed RK2 copied prescriptions v1";
    for (int kind = 0; kind < 3; ++kind) {
        const auto name = "managed-rk2-forcing-corrupt-" + std::to_string(kind);
        SWMMEngine target;
        ASSERT_NO_THROW(startManagedRk2Session(target, name));
        auto router = nested(payload, router_magic).second, body = surfacePayload(router);
        const auto marker = body.find(record);
        ASSERT_NE(marker, std::string::npos);
        const auto offset = marker + record.size() + 3 * sizeof(uint64_t);
        ASSERT_LT(offset + 2 * sizeof(int), body.size());
        int value = kind == 0 ? 0 : kind == 1 ? 5 : 999;
        std::memcpy(body.data() + offset + (kind == 2 ? sizeof(int) : 0), &value, sizeof(value));
        router = withSurfacePayload(router, body);
        auto changed = replaceNested(payload, router_magic, router);
        sum = crc(changed);
        detail::CompletedSourceArchive out;
        out(magic, version, model, sum, changed);
        const auto bad = root() / (name + "-bad.chk"), before = root() / (name + "-before.chk"),
                   after = root() / (name + "-after.chk");
        write(bad, out.bytes());
        ASSERT_EQ(target.saveCheckpoint(before.string()), "");
        EXPECT_EQ(target.loadCheckpoint(bad.string()),
                  "Invalid RK2 runtime forcing prescription or paired groundwater owner.");
        ASSERT_EQ(target.saveCheckpoint(after.string()), "");
        EXPECT_TRUE(bytes(before) == bytes(after));
        ASSERT_EQ(target.loadCheckpoint(good.string()), "");
        compare(source, target, name + "-retry");
    }
}
TEST(EngineCheckpoint, ManagedRk2PendingBatchPreservesTheAuthoredCouplingCadence) {
    SWMMEngine source, target;
    const std::string extra = "[2D_OPTIONS]\nCOUPLING_SYNC .7\n";
    ASSERT_NO_THROW(startManagedRk2Session(source, "managed-rk2-pending-source", extra));
    ASSERT_NO_THROW(startManagedRk2Session(target, "managed-rk2-pending-target", extra));
    ASSERT_NO_THROW(step(source));
    EXPECT_GT(source.context().current_time, 0);
    EXPECT_EQ(source.surfaceRouter2D().subsurface().cellCompletedTime(0), 0);
    const auto saved = root() / "managed-rk2-pending.chk";
    ASSERT_EQ(source.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(target.loadCheckpoint(saved.string()), "");
    compare(source, target, "managed-rk2-pending-restored");
    while (source.context().elapsed_ms < source.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(source));
        ASSERT_NO_THROW(step(target));
        compare(source, target, "managed-rk2-pending-continued");
    }
    EXPECT_EQ(source.surfaceRouter2D().options().coupling_sync, .7);
}
namespace {
/**
 * @brief Build three hourly meteorology rows spanning the managed RK2 midnight boundary.
 *
 * Uses fixed surrounding rows and std::to_string for the requested shortwave value; does not
 * validate weather ranges, create files or parse records.
 *
 * @param[in] last_shortwave Shortwave radiation in W/m2 for the midnight (middle) row, despite the
 *     historical parameter name.
 * @return Owned meteorology text with C temperature, percent RH, m/s wind and W/m2 short/longwave
 *     columns.
 */
std::string rk2MetText(double last_shortwave = 800) {
    return "01/31/2026 23:00 4 40 1 200 300\n02/01/2026 00:00 9 75 3 " +
           std::to_string(last_shortwave) + " 375\n02/01/2026 01:00 -2 60 2 100 290\n";
}
/**
 * @brief Build the coupled thermal/reaction appendix for cached hourly-weather continuation.
 *
 * Enables surface/radiative exchange and authors constant evaporation 18 in the CMS project
 * convention. Does not create the meteorology file, initialize a site or validate configuration.
 * @return Owned SIGMA/layered/porous managedRk2Quality text plus coupled surface/ground heat,
 *     climate evaporation and surface-water thermal boundary options.
 */
std::string rk2MetHeat() {
    return managedRk2Quality("SIGMA", true, true, true, true) +
           "[2D_OPTIONS]\nHEAT_MODEL COUPLED\nBED_HEAT_MODEL GROUND_COLUMN\nHEAT_SURFACE_EXCHANGE YES\nHEAT_RADIATIVE_EXCHANGE YES\nEVAPORATION CLIMATE\n[EVAPORATION]\nCONSTANT 18\n[GW_TRANSPORT_OPTIONS]\nSURFACE_THERMAL_BC SURFACE_WATER\n";
}
}
TEST(EngineCheckpoint, ManagedRk2CachedMeteorologyOwnsHourlyWeatherAcrossMidnightWithoutReread) {
    for (bool heat : {false, true}) {
        const auto name = "managed-rk2-met-owned-" + std::to_string(heat), met = name + ".met";
        write(root() / met, rk2MetText());
        SWMMEngine a, b;
        const auto configure = [&](SWMMEngine& e) {
            e.context().snowpacks.config.met_file.original = met;
        };
        const auto extra = (heat ? rk2MetHeat() : std::string()) +
                           "[OPTIONS]\nWET_STEP 00:00:01\nDRY_STEP 00:00:01\n";
        ASSERT_NO_THROW(
            startManagedRk2Session(a, name + "-a", extra, SWMM_CHECKPOINT_OUTPUT_NONE, configure));
        ASSERT_NO_THROW(
            startManagedRk2Session(b, name + "-b", extra, SWMM_CHECKPOINT_OUTPUT_NONE, configure));
        ASSERT_EQ(a.context().snowpacks.meteorology.records().size(), 3u);
        std::filesystem::remove(root() / met);
        double actual = 0;
        ASSERT_EQ(swmm_coupling_advance_to(&a, 1.237, &actual), SWMM_OK);
        const auto saved = root() / (name + ".chk");
        ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
        ASSERT_EQ(b.loadCheckpoint(saved.string()), "");
        compare(a, b, name + "-restored");
        while (a.context().current_time < 4.7) {
            ASSERT_NO_THROW(step(a));
            ASSERT_NO_THROW(step(b));
            compare(a, b, name + "-continued");
        }
        EXPECT_NEAR(a.context().climate_state.temperature, 48.2, 1e-10);
        if (heat) {
            EXPECT_DOUBLE_EQ(a.context().climate_state.radiation.shortwave, 800);
            EXPECT_DOUBLE_EQ(a.context().climate_state.radiation.longwave, 375);
            EXPECT_TRUE(a.context().climate_state.radiation.shared_heat_forcing);
        }
    }
}
TEST(EngineCheckpoint, ManagedRk2ChangedCachedWeatherRejectsBeforeTargetMutation) {
    for (bool heat : {false, true})
        for (bool future : {false, true}) {
            const auto name = "managed-rk2-met-changed-" + std::to_string(heat) +
                              std::to_string(future),
                       met = name + ".met";
            write(root() / met, rk2MetText());
            SWMMEngine a, b;
            const auto configure = [&](SWMMEngine& e) {
                e.context().snowpacks.config.met_file.original = met;
            };
            const auto extra = (heat ? rk2MetHeat() : std::string()) +
                               "[OPTIONS]\nWET_STEP 00:00:01\nDRY_STEP 00:00:01\n";
            ASSERT_NO_THROW(startManagedRk2Session(a, name + "-a", extra,
                                                   SWMM_CHECKPOINT_OUTPUT_NONE, configure));
            auto text = rk2MetText(future ? 800 : 801);
            if (future)
                text.replace(text.find("100 290"), 7, "101 290");
            write(root() / met, text);
            ASSERT_NO_THROW(startManagedRk2Session(b, name + "-b", extra,
                                                   SWMM_CHECKPOINT_OUTPUT_NONE, configure));
            ASSERT_NO_THROW(step(a));
            const auto saved = root() / (name + ".chk"), before = root() / (name + "-before.chk");
            ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
            ASSERT_EQ(b.saveCheckpoint(before.string()), "");
            const auto initial = bytes(before);
            EXPECT_FALSE(b.loadCheckpoint(saved.string()).empty());
            ASSERT_EQ(b.saveCheckpoint(before.string()), "");
            EXPECT_EQ(bytes(before), initial);
        }
}
TEST(EngineCheckpoint, ManagedRk2QualityBinaryCompanionsRestoreActualReportsAndPhysicalState) {
    for (int output : {SWMM_CHECKPOINT_OUTPUT_BINARY, SWMM_CHECKPOINT_OUTPUT_COMBINED})
        for (bool met : {false, true}) {
#ifndef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
            if (output == SWMM_CHECKPOINT_OUTPUT_COMBINED)
                continue;
#endif
            const auto name = "managed-rk2-binary-quality-" + std::to_string(output) +
                              std::to_string(met),
                       file = name + ".met";
            if (met)
                write(root() / file, rk2MetText());
            SWMMEngine a, b;
            const auto configure = [&](SWMMEngine& e) {
                if (met)
                    e.context().snowpacks.config.met_file.original = file;
            };
            const auto extra =
                rk2MetHeat() +
                "[OPTIONS]\nWET_STEP 00:00:01\nDRY_STEP 00:00:01\n[2D_OPTIONS]\nCOUPLING_SYNC .7\nREPORT_2D_STEP 00:00:01\n";
            ASSERT_NO_THROW(startManagedRk2Session(a, name + "-a", extra, output, configure));
            ASSERT_NO_THROW(startManagedRk2Session(b, name + "-b", extra, output, configure));
            double actual = 0;
            ASSERT_EQ(swmm_coupling_advance_to(&a, 1.237, &actual), SWMM_OK);
            const auto saved = root() / (name + ".chk");
            ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
            ASSERT_EQ(b.loadCheckpoint(saved.string()), "");
            compareManagedRk2(a, b, name + "-restored", output == SWMM_CHECKPOINT_OUTPUT_COMBINED);
            while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
                ASSERT_NO_THROW(step(a));
                ASSERT_NO_THROW(step(b));
                compareManagedRk2(a, b, name + "-continued",
                                  output == SWMM_CHECKPOINT_OUTPUT_COMBINED);
            }
            ASSERT_EQ(a.end(), SWMM_OK);
            ASSERT_EQ(b.end(), SWMM_OK);
            sameBinary(name + "-a", name + "-b", 5);
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
            if (output == SWMM_CHECKPOINT_OUTPUT_COMBINED)
                sameMesh(name + "-a", name + "-b", 5);
#endif
        }
}
TEST(EngineCheckpoint, ManagedRk2QualityCombinedMismatchPreservesBothOutputPrefixes) {
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
    SWMMEngine a, b;
    auto changed = rk2MetHeat();
    const auto pos = changed.find("-.01*A");
    ASSERT_NE(pos, std::string::npos);
    changed.replace(pos, 6, "-.02*A");
    ASSERT_NO_THROW(startManagedRk2Session(a, "managed-rk2-combined-model-a", rk2MetHeat(),
                                           SWMM_CHECKPOINT_OUTPUT_COMBINED));
    ASSERT_NO_THROW(startManagedRk2Session(b, "managed-rk2-combined-model-b", changed,
                                           SWMM_CHECKPOINT_OUTPUT_COMBINED));
    ASSERT_NO_THROW(step(a));
    const auto saved = root() / "managed-rk2-combined-model.chk",
               before = root() / "managed-rk2-combined-before.chk";
    ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.saveCheckpoint(before.string()), "");
    const auto initial = bytes(before), binary = bytes(root() / "managed-rk2-combined-model-b.out"),
               mesh = bytes(root() / "managed-rk2-combined-model-b.h5");
    EXPECT_FALSE(b.loadCheckpoint(saved.string()).empty());
    ASSERT_EQ(b.saveCheckpoint(before.string()), "");
    EXPECT_EQ(bytes(before), initial);
    EXPECT_EQ(bytes(root() / "managed-rk2-combined-model-b.out"), binary);
    EXPECT_EQ(bytes(root() / "managed-rk2-combined-model-b.h5"), mesh);
#endif
}
TEST(EngineCheckpoint, ManagedRk2SnowWaterRestoresActualPointOwnerPendingBatchAndAllOutputs) {
    for (const std::string model : {"DEGREE_DAY", "UEB"})
        for (int output : {0, 1, 2, 3}) {
#ifndef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
            if (output >= 2)
                continue;
#endif
            const auto name = "managed-rk2-snow-" + model + std::to_string(output),
                       met = name + ".met";
            SCOPED_TRACE(name);
            write(root() / met, rk2MetText());
            SWMMEngine a, b;
            const auto configure = [&](SWMMEngine& e) {
                e.context().snowpacks.config.met_file.original = met;
            };
            const auto extra =
                "[OPTIONS]\nIGNORE_SNOWMELT NO\nWET_STEP 00:00:01\nDRY_STEP 00:00:01\n[TEMPERATURE]\nSNOWMELT_MODEL " +
                model +
                "\nSITE 45 -120 -8 1000\n[SNOWPACKS]\nSP PERVIOUS 1 2 0 .1 1000 100 0\n[2D_SNOW_DEFAULTS]\n* SNOWPACK SP\n[2D_OPTIONS]\nCOUPLING_SYNC .7\nREPORT_2D_STEP 00:00:01\nREPORT_2D_VARIABLES DEPTH GROUNDWATER RAINFALL\n";
            ASSERT_NO_THROW(startManagedRk2Session(a, name + "-a", extra, output, configure));
            ASSERT_NO_THROW(startManagedRk2Session(b, name + "-b", extra, output, configure));
            double actual = 0;
            ASSERT_EQ(swmm_coupling_advance_to(&a, 1.237, &actual), SWMM_OK);
            const auto saved = root() / (name + ".chk");
            ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
            ASSERT_EQ(b.loadCheckpoint(saved.string()), "");
            EXPECT_EQ(a.surfaceRouter2D().state().snow.saveCheckpoint(),
                      b.surfaceRouter2D().state().snow.saveCheckpoint());
            compareManagedRk2(a, b, name + "-restored", output >= 2);
            while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
                ASSERT_NO_THROW(step(a));
                ASSERT_NO_THROW(step(b));
                compareManagedRk2(a, b, name + "-continued", output >= 2);
            }
            for (const auto& book : a.surfaceRouter2D().state().snow.ledgers()) {
                EXPECT_GT(book.liquid, 0);
                EXPECT_NEAR(book.residual(), 0, 1e-10);
                EXPECT_NEAR(book.energyResidual(), 0, 1e-7);
            }
            ASSERT_EQ(a.end(), SWMM_OK);
            ASSERT_EQ(b.end(), SWMM_OK);
            if (output == 1 || output == 3)
                sameBinary(name + "-a", name + "-b", 5);
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
            if (output >= 2)
                sameMesh(name + "-a", name + "-b", 5);
#endif
        }
}
TEST(EngineCheckpoint, ManagedRk2FileRadiationRefusesUnreadableCache) {
    SWMMEngine engine;
    const auto configure = [](SWMMEngine& e) {
        e.context().snowpacks.config.met_file.original = "unqualified-met.csv";
    };
    EXPECT_THROW(startManagedRk2Session(engine, "managed-rk2-met-refusal", "",
                                        SWMM_CHECKPOINT_OUTPUT_NONE, configure),
                 std::runtime_error);
    EXPECT_EQ(engine.context().current_time, 0);
    EXPECT_FALSE(std::filesystem::exists(root() / "managed-rk2-met-refusal.h5"));
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, ManagedRk2SurfaceBoundariesReplayQualityClearAndPendingReportsExactly) {
    for (bool head : {false, true})
        for (bool cleared : {false, true})
            for (bool porous : {false, true}) {
                const auto name = "managed-rk2-surface-edge-" + std::to_string(head) +
                                  std::to_string(cleared) + std::to_string(porous);
                SWMMEngine a, b;
                const auto extra = managedRk2Quality("SIGMA", true, true, porous, true);
                ASSERT_NO_THROW(startManagedRk2Session(a, name + "-a", extra));
                ASSERT_NO_THROW(startManagedRk2Session(b, name + "-b", extra));
                const auto& tr = a.surfaceRouter2D().state().transport;
                std::vector<double> quality(tr.n_species, 3);
                quality[tr.temp_row] = -8;
                quality[tr.age_row] = 2;
                int edge = -1;
                const auto& mesh = a.surfaceRouter2D().mesh();
                for (int k = 0; k < mesh.cell_vertex_count(0); ++k)
                    if (mesh.cell_neighbour(0, k) < 0) {
                        edge = k;
                        break;
                    }
                ASSERT_GE(edge, 0);
                SWMM_CouplingBoundary boundary{
                    sizeof(boundary), 0, 0, edge, head ? 1 : 2, head ? .56 : -.001, tr.n_species,
                    quality.data()};
                SWMM_CouplingFrame frame{sizeof(frame), 1, "surface", 0, nullptr, 1,
                                         &boundary,     0, nullptr,   0, nullptr};
                unsigned flags = 0;
                ASSERT_EQ(swmm_coupling_capabilities(&a, &flags), SWMM_OK);
                EXPECT_NE(flags & 8, 0);
                ASSERT_EQ(swmm_coupling_apply_frame(&a, &frame), SWMM_OK);
                double actual = 0;
                ASSERT_EQ(swmm_coupling_advance_to(&a, 1.237, &actual), SWMM_OK);
                if (cleared) {
                    boundary.kind = 0;
                    boundary.species_count = 0;
                    boundary.concentrations = nullptr;
                    ASSERT_EQ(swmm_coupling_apply_frame(&a, &frame), SWMM_OK);
                }
                const auto saved = root() / (name + ".chk");
                ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
                ASSERT_EQ(b.loadCheckpoint(saved.string()), "");
                compare(a, b, name + "-restored");
                while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
                    ASSERT_NO_THROW(step(a));
                    ASSERT_NO_THROW(step(b));
                    compare(a, b, name + "-continued");
                }
                const auto& receipt =
                    a.surfaceRouter2D().boundary().receipts.at(twoD::MeshData::slot(0, edge));
                EXPECT_GT(receipt.water_in, 0);
                EXPECT_GT(receipt.heat_applied < 0 ? -receipt.heat_applied : receipt.heat_applied,
                          0);
            }
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, ManagedRk2SurfaceLatentNativeAndCopiedDemandContinueAcrossMidnight) {
    for (const auto& layout : std::vector<std::pair<std::string, bool>>{
             {"CLOSED_FORM", false}, {"ENSLAVED", false}, {"SIGMA", false}, {"SIGMA", true}})
        for (bool copied : {false, true}) {
            const auto name = "managed-rk2-latent-" + layout.first + std::to_string(layout.second) +
                              std::to_string(copied);
            SWMMEngine a, b;
            const auto extra =
                managedRk2Quality(layout.first, layout.second, true, true, true) +
                "[2D_OPTIONS]\nHEAT_MODEL COUPLED\nBED_HEAT_MODEL GROUND_COLUMN\nHEAT_SURFACE_EXCHANGE YES\nHEAT_RADIATIVE_EXCHANGE YES\nEVAPORATION CLIMATE\n[EVAPORATION]\nCONSTANT 18\n[GW_TRANSPORT_OPTIONS]\nSURFACE_THERMAL_BC SURFACE_WATER\n";
            ASSERT_NO_THROW(startManagedRk2Session(a, name + "-a", extra));
            ASSERT_NO_THROW(startManagedRk2Session(b, name + "-b", extra));
            if (copied) {
                SWMM_CouplingForcing demand{sizeof(demand), 2, -1, 1, .0002, 1.237, 0, nullptr};
                SWMM_CouplingFrame frame{sizeof(frame), 1, "latent", 0, nullptr, 0,
                                         nullptr,       1, &demand,  0, nullptr};
                ASSERT_EQ(swmm_coupling_apply_frame(&a, &frame), SWMM_OK);
            }
            double actual = 0;
            ASSERT_EQ(swmm_coupling_advance_to(&a, 1, &actual), SWMM_OK);
            const auto saved = root() / (name + ".chk");
            ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
            ASSERT_EQ(b.loadCheckpoint(saved.string()), "");
            compare(a, b, name + "-restored");
            while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
                ASSERT_NO_THROW(step(a));
                ASSERT_NO_THROW(step(b));
                compare(a, b, name + "-continued");
            }
            EXPECT_LT(std::accumulate(a.surfaceRouter2D().state().heat.latent_j.begin(),
                                      a.surfaceRouter2D().state().heat.latent_j.end(), 0.),
                      0);
        }
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, ManagedRk2AuthoredSurfaceFlowSeriesContinuesAcrossMidnight) {
    SWMMEngine a, b;
    const auto extra =
        managedRk2Quality("SIGMA", true, true, true, true) +
        "[2D_BOUNDARY_CONDITIONS]\n0 1 TS_FLOW SurfaceFlow\n[2D_BOUNDARY_QUALITY]\n0 1 Dye 3\n0 1 A 1\n0 1 B 0\n0 1 __TEMPERATURE__ -8\n0 1 __WATER_AGE__ 2\n"
        "[TIMESERIES]\nSurfaceFlow 01/31/2026 23:59:58 -.00001\nSurfaceFlow 01/31/2026 23:59:59 .00002\nSurfaceFlow 02/01/2026 00:00:01 -.00001\nSurfaceFlow 02/01/2026 00:00:03 .00001\n";
    ASSERT_NO_THROW(startManagedRk2Session(a, "managed-rk2-surface-series-a", extra));
    ASSERT_NO_THROW(startManagedRk2Session(b, "managed-rk2-surface-series-b", extra));
    double actual = 0;
    ASSERT_EQ(swmm_coupling_advance_to(&a, .735, &actual), SWMM_OK);
    const auto saved = root() / "managed-rk2-surface-series.chk";
    ASSERT_EQ(a.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.loadCheckpoint(saved.string()), "");
    compare(a, b, "managed-rk2-surface-series-restored");
    while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(a));
        ASSERT_NO_THROW(step(b));
        compare(a, b, "managed-rk2-surface-series-continued");
    }
    const auto& receipt = a.surfaceRouter2D().boundary().receipts.at(twoD::MeshData::slot(0, 1));
    EXPECT_GT(receipt.water_in, 0);
    EXPECT_GT(receipt.water_out, 0);
}
#endif

namespace {
/**
 * @brief Build a closed-storage or connected-network pollutant checkpoint fixture.
 *
 * Raw decay is formatted without range validation. Mesh mode reduces storage depth/initial depth
 * and appends native mesh/GW transport rows. No file IO or simulation occurs; the disconnected CFS
 * model is a distinct authored fixture, not a unit-equivalent conversion.
 *
 * @param[in] connected True retains CMS storage/conduit/outfall; false selects CFS and removes the
 *     conduit section.
 * @param[in] mesh Add two-cell SI surface/GW coupling and different pollutant seeds when true.
 * @param[in] decay Authored first-order Tracer decay coefficient in 1/day.
 * @return Owned five-second cross-midnight deck with Dye mg/L and Tracer ug/L plus independent
 *     initial/inflow values.
 */
std::string networkPollutantDeck(bool connected = false, bool mesh = false, double decay = .03) {
    std::string text =
        "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nIGNORE_QUALITY NO\nSTART_DATE 01/31/2026\nSTART_TIME 23:59:58\nEND_DATE 02/01/2026\nEND_TIME 00:00:03\nREPORT_START_DATE 01/31/2026\nREPORT_START_TIME 23:59:58\nROUTING_STEP .1\nREPORT_STEP 00:00:01\nSKIP_STEADY_STATE NO\n[STORAGE]\nS 0 3 .6 FUNCTIONAL 0 0 20 0 0\n[POLLUTANTS]\nDye MG/L 0 0 0 0 NO * 0 0 2\nTracer UG/L 0 0 0 " +
        std::to_string(decay) +
        " NO * 0 0 7\n[INFLOWS]\nS FLOW Q FLOW 1 1 0\nS Dye D CONCEN 1 1 0\nS Tracer T CONCEN 1 1 0\n[TIMESERIES]\nQ 01/31/2026 23:59:58 .001\nQ 02/01/2026 00:00:03 .001\nD 01/31/2026 23:59:58 3\nD 02/01/2026 00:00:03 3\nT 01/31/2026 23:59:58 5\nT 02/01/2026 00:00:03 5\n";
    text += "[OUTFALLS]\nO 0 FIXED .1 NO\n";
    text += "[CONDUITS]\nC S O 30 .013 " + std::string(connected ? ".2" : "2") +
            " 0 0 0\n[XSECTIONS]\nC CIRCULAR .15 0 0 0\n";
    if (!connected) {
        text.replace(text.find("O 0 FIXED .1"), std::string("O 0 FIXED .1").size(), "O 0 FIXED 0");
        text.replace(text.find("FLOW_UNITS CMS"), std::string("FLOW_UNITS CMS").size(),
                     "FLOW_UNITS CFS");
    }
    if (!connected)
        text.erase(text.find("[CONDUITS]"));
    if (mesh)
        text.replace(text.find("S 0 3 .6"), std::string("S 0 3 .6").size(), "S 0 .2 .15");
    if (mesh)
        text +=
            "[2D_OPTIONS]\nREPORT_2D NO\nMAX_TIMESTEP .02\nLTS_TIERS 1\nCOUPLING_SYNC .17\nTRANSPORT_POLLUTANTS YES\nEVAPORATION NO\n[2D_VERTICES]\n0 0 .5\n4 0 .5\n0 2 .5\n4 2 .5\n[2D_TRIANGLES]\n0 1 2 .03 .04\n1 3 2 .03 .03\n[2D_VERTEX_NODE_MAP]\n0 S .7 .01\n[2D_AQUIFER_OPTIONS]\nCLOSURE CLOSED_FORM\nNODE_ENROLMENT ROWS\nLINK_SEEPAGE NONE\nGW_ET NONE\n[2D_AQUIFER]\n* 36 1 .45 .1 2 HG0 .3 C_LOSS .02\n[2D_INITIAL_QUALITY]\n* Dye 9\n* Tracer 11\n[GW_INITIAL_QUALITY]\n* SAT Dye 2\n* UNSAT Dye 4\n* SAT Tracer 3\n* UNSAT Tracer 5\n";
    return text;
}
/**
 * @brief Author and start the managed network pollutant continuation profile.
 *
 * Writes networkPollutantDeck plus selected output rows, opens then calls startCheckpointSession.
 * File writes are unchecked; output/mesh incompatibility or lifecycle rejection throws with partial
 * e retained for caller cleanup. Does not step/end/report here.
 *
 * @param[in,out] e Caller-owned openable engine; managed session starts on success.
 * @param[in] name Raw root-relative artifact stem; matching input/report/result files may be
 *     overwritten.
 * @param[in] output Public output mode: 1/3 requests binary, >=2 appends FLOAT64 mesh output rows.
 * @param[in] connected Select connected CMS network rather than disconnected CFS storage.
 * @param[in] mesh Include two-cell surface/GW coupling in the authored deck.
 * @param[in] decay Tracer authoring decay coefficient in 1/day.
 */
void startNetworkPollutants(SWMMEngine& e, const std::string& name, int output = 0,
                            bool connected = false, bool mesh = false, double decay = .03) {
    const auto input = root() / (name + ".inp"), report = root() / (name + ".rpt"),
               binary = root() / (name + ".out");
    auto text = networkPollutantDeck(connected, mesh, decay);
    if (output >= 2)
        text +=
            "[2D_OPTIONS]\nREPORT_2D YES\nOUTPUT_PRECISION FLOAT64\nREPORT_2D_STEP 00:00:01\nREPORT_2D_VARIABLES DEPTH GROUNDWATER\nOUTPUT_FILE \"" +
            (root() / (name + ".h5")).string() + "\"\n";
    write(input, text);
    if (e.open(input.string().c_str(), report.string().c_str(),
               (output == 1 || output == 3) ? binary.string().c_str() : nullptr))
        throw std::runtime_error(e.context().error_message);
    if (auto error = e.startCheckpointSession(output, {}, false, {}); !error.empty())
        throw std::runtime_error(error);
}
}
TEST(EngineCheckpoint, NetworkPollutantsClosedStoragePreservesIndependentMassAndCurrentOldQuality) {
    for (int output : {0, 1}) {
        const auto name = "network-quality-storage-" + std::to_string(output);
        SWMMEngine a, b;
        ASSERT_NO_THROW(startNetworkPollutants(a, name + "-a", output));
        ASSERT_NO_THROW(startNetworkPollutants(b, name + "-b", output));
        // Native DW has a trapezoidal startup with no preceding inflow.
        // Freeze the filling reference after that first step; CFS is exact.
        ASSERT_NO_THROW(step(a));
        const auto& initial = a.context();
        const int node = initial.node_names.find("S"), row = node * initial.n_pollutants();
        const double mass = initial.nodes.conc[row] * initial.nodes.volume[node],
                     origin = initial.current_time;
        EXPECT_GT(mass, 0);
        while (a.context().current_time < 1.237)
            ASSERT_NO_THROW(step(a));
        const auto file = root() / (name + ".chk");
        ASSERT_EQ(a.saveCheckpoint(file.string()), "");
        ASSERT_EQ(b.loadCheckpoint(file.string()), "");
        compare(a, b, name + "-restored");
        while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a));
            ASSERT_NO_THROW(step(b));
            compare(a, b, name + "-continued");
            const auto& c = a.context();
            EXPECT_EQ(c.nodes.conc, b.context().nodes.conc);
            EXPECT_EQ(c.nodes.conc_old, b.context().nodes.conc_old);
            EXPECT_NEAR(c.nodes.conc[row] * c.nodes.volume[node],
                        mass + 3 * .001 * (c.current_time - origin), 1e-9);
        }
        ASSERT_EQ(a.end(), 0);
        ASSERT_EQ(b.end(), 0);
        if (output == 1)
            EXPECT_TRUE(bytes(root() / (name + "-a.out")) == bytes(root() / (name + "-b.out")));
    }
}
TEST(EngineCheckpoint, NetworkPollutantsConduitContinuesLoadsStatisticsAndBinaryHistory) {
    SWMMEngine a, b;
    ASSERT_NO_THROW(startNetworkPollutants(a, "network-quality-conduit-a", 1, true));
    ASSERT_NO_THROW(startNetworkPollutants(b, "network-quality-conduit-b", 1, true));
    while (a.context().current_time < 1.237)
        ASSERT_NO_THROW(step(a));
    const auto file = root() / "network-quality-conduit.chk";
    ASSERT_EQ(a.saveCheckpoint(file.string()), "");
    ASSERT_EQ(b.loadCheckpoint(file.string()), "");
    while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
        ASSERT_NO_THROW(step(a));
        ASSERT_NO_THROW(step(b));
        compare(a, b, "network-quality-conduit-continued");
        EXPECT_EQ(a.context().links.conc, b.context().links.conc);
        EXPECT_EQ(a.context().links.conc_old, b.context().links.conc_old);
        EXPECT_EQ(a.context().links.stat_total_load, b.context().links.stat_total_load);
    }
    ASSERT_EQ(a.end(), 0);
    ASSERT_EQ(b.end(), 0);
    EXPECT_TRUE(bytes(root() / "network-quality-conduit-a.out") ==
                bytes(root() / "network-quality-conduit-b.out"));
}
TEST(EngineCheckpoint,
     NetworkPollutantsChangedUnitsDecayAndInitialIdentityPreserveFreshTargetAndOutput) {
    SWMMEngine source;
    ASSERT_NO_THROW(startNetworkPollutants(source, "network-quality-identity-source", 1, true));
    while (source.context().current_time < 1.237)
        ASSERT_NO_THROW(step(source));
    const auto good = root() / "network-quality-identity.chk";
    ASSERT_EQ(source.saveCheckpoint(good.string()), "");
    for (int change = 0; change < 4; ++change) {
        const auto name = "network-quality-identity-" + std::to_string(change);
        SWMMEngine target;
        auto text = networkPollutantDeck(true, false, change == 0 ? .04 : .03);
        if (change == 1)
            text.replace(text.find("Dye MG/L"), std::string("Dye MG/L").size(), "Dye UG/L");
        if (change == 2)
            text += "[INITIAL_QUALITY]\nNODE S Dye 12\n";
        if (change == 3) {
            const auto p = text.find("Dye MG/L"), q = text.find("[INFLOWS]", p);
            text.replace(p, q - p,
                         "Tracer UG/L 0 0 0 .03 NO * 0 0 7\nDye MG/L 0 0 0 0 NO * 0 0 2\n");
        }
        write(root() / (name + ".inp"), text);
        ASSERT_EQ(target.open((root() / (name + ".inp")).string().c_str(),
                              (root() / (name + ".rpt")).string().c_str(),
                              (root() / (name + ".out")).string().c_str()),
                  0);
        ASSERT_EQ(target.startCheckpointSession(1, {}, false, {}), "");
        const auto before = root() / (name + "-before.chk"), after = root() / (name + "-after.chk");
        ASSERT_EQ(target.saveCheckpoint(before.string()), "");
        const auto prefix = bytes(root() / (name + ".out"));
        EXPECT_FALSE(target.loadCheckpoint(good.string()).empty());
        ASSERT_EQ(target.saveCheckpoint(after.string()), "");
        EXPECT_TRUE(bytes(before) == bytes(after));
        EXPECT_TRUE(prefix == bytes(root() / (name + ".out")));
    }
}
TEST(EngineCheckpoint, NetworkPollutantsValidChecksumNegativeConcentrationIsAtomicAndAllowsRetry) {
    SWMMEngine a, b;
    ASSERT_NO_THROW(startNetworkPollutants(a, "network-quality-corrupt-a"));
    ASSERT_NO_THROW(startNetworkPollutants(b, "network-quality-corrupt-b"));
    while (a.context().current_time < 1.237)
        ASSERT_NO_THROW(step(a));
    const auto good = root() / "network-quality-corrupt-good.chk",
               bad = root() / "network-quality-corrupt-bad.chk",
               before = root() / "network-quality-corrupt-before.chk",
               after = root() / "network-quality-corrupt-after.chk";
    ASSERT_EQ(a.saveCheckpoint(good.string()), "");
    using Archive = detail::CompletedSourceArchive;
    std::string magic, model, payload;
    uint32_t version = 0, sum = 0;
    Archive file(bytes(good));
    file(magic, version, model, sum, payload);
    file.finish();
    const std::string hydraulic_magic = "OpenSWMM accepted DW hydraulic boundary";
    const auto tag = payload.find(hydraulic_magic);
    ASSERT_NE(tag, std::string::npos);
    ASSERT_GE(tag, 16u);
    const auto at = tag - 16;
    uint64_t size = 0;
    std::memcpy(&size, payload.data() + at, sizeof(size));
    ASSERT_LE(size, payload.size() - at - 8);
    auto hydraulic = payload.substr(at + 8, size);
    std::string hm, hmodel, body;
    uint32_t hv = 0, hsum = 0;
    Archive in(hydraulic);
    in(hm, hv, hmodel, hsum, body);
    in.finish();
    const auto node = a.context().node_names.find("S");
    const double value = a.context().nodes.conc[node * a.context().n_pollutants()];
    const std::string needle(reinterpret_cast<const char*>(&value), sizeof(value));
    const auto position = body.find(needle);
    ASSERT_NE(position, std::string::npos);
    ASSERT_EQ(body.find(needle, position + 1), std::string::npos);
    const double negative = -value;
    std::memcpy(body.data() + position, &negative, sizeof(negative));
    hsum = crc(body);
    Archive hout;
    hout(hm, hv, hmodel, hsum, body);
    Archive record;
    auto encoded = hout.bytes();
    record(encoded);
    payload.replace(at, size + 8, record.bytes());
    sum = crc(payload);
    Archive out;
    out(magic, version, model, sum, payload);
    write(bad, out.bytes());
    ASSERT_EQ(b.saveCheckpoint(before.string()), "");
    EXPECT_FALSE(b.loadCheckpoint(bad.string()).empty());
    ASSERT_EQ(b.saveCheckpoint(after.string()), "");
    EXPECT_TRUE(bytes(before) == bytes(after));
    ASSERT_EQ(b.loadCheckpoint(good.string()), "");
    compare(a, b, "network-quality-corrupt-retry");
}
#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, NetworkPollutantsFirstOrderSharedMeshOwnsPhysicalQueuesAndRestart) {
    for (int output : {0, 1, 2, 3}) {
#ifndef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
        if (output >= 2)
            continue;
#endif
        const auto name = "network-quality-shared-" + std::to_string(output);
        SWMMEngine a, b;
        ASSERT_NO_THROW(startNetworkPollutants(a, name + "-a", output, true, true, 0));
        ASSERT_NO_THROW(startNetworkPollutants(b, name + "-b", output, true, true, 0));
        while (a.context().current_time < 1.237)
            ASSERT_NO_THROW(step(a));
        const auto file = root() / (name + ".chk");
        ASSERT_EQ(a.saveCheckpoint(file.string()), "");
        ASSERT_EQ(b.loadCheckpoint(file.string()), "");
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
        if (output >= 2)
            compareMesh(a, b, name + "-restored");
        else
#endif
            compare(a, b, name + "-restored");
        bool transferred = false;
        while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a));
            ASSERT_NO_THROW(step(b));
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
            if (output >= 2)
                compareMesh(a, b, name + "-continued");
            else
#endif
                compare(a, b, name + "-continued");
            EXPECT_EQ(a.context().nodes.conc, b.context().nodes.conc);
            EXPECT_EQ(a.context().nodes.coupling_qual_queue, b.context().nodes.coupling_qual_queue);
            const auto& tr = a.surfaceRouter2D().state().transport;
            transferred |= tr.lost_coupling[0] != 0 || tr.gained_coupling[0] != 0;
        }
        EXPECT_TRUE(transferred);
        ASSERT_EQ(a.end(), 0);
        ASSERT_EQ(b.end(), 0);
        if (output == 1 || output == 3)
            sameBinary(name + "-a", name + "-b", 5);
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
        if (output >= 2)
            sameMesh(name + "-a", name + "-b", 5);
#endif
    }
}
#endif

TEST(EngineCheckpoint, NetworkPollutantsSignedNativeLoadsContinueAcrossReversal) {
    const auto text0 = networkPollutantDeck(true);
    auto text = text0;
    text.replace(text.find("S Dye D CONCEN"), std::string("S Dye D CONCEN").size(), "S Dye D MASS");
    const auto begin = text.find("D 01/31/2026"), end = text.find("T 01/31/2026", begin);
    text.replace(
        begin, end - begin,
        "D 01/31/2026 23:59:58 -10\nD 02/01/2026 00:00:00 -10\nD 02/01/2026 00:00:01 20\nD 02/01/2026 00:00:03 20\n");
    SWMMEngine a, b;
    int id = 0;
    for (auto* e : {&a, &b}) {
        const auto name = "network-quality-signed-" + std::to_string(id++);
        write(root() / (name + ".inp"), text);
        ASSERT_EQ(e->open((root() / (name + ".inp")).string().c_str(),
                          (root() / (name + ".rpt")).string().c_str(),
                          (root() / (name + ".out")).string().c_str()),
                  0);
        ASSERT_EQ(e->startCheckpointSession(1, {}, false, {}), "");
    }
    while (a.context().current_time < 1.237)
        ASSERT_NO_THROW(step(a));
    ASSERT_LT(a.context().mass_balance.qual_routing_ex_in[0], 0);
    const auto good = root() / "network-quality-signed.chk";
    ASSERT_EQ(a.saveCheckpoint(good.string()), "");
    ASSERT_EQ(b.loadCheckpoint(good.string()), "");
    bool positive = false, negative = false;
    while (a.context().elapsed_ms < a.context().options.totalDurationMs()) {
        const double before = a.context().mass_balance.qual_routing_ex_in[0];
        ASSERT_NO_THROW(step(a));
        ASSERT_NO_THROW(step(b));
        compare(a, b, "network-quality-signed-continued");
        const double delta = a.context().mass_balance.qual_routing_ex_in[0] - before;
        positive |= delta > 0;
        negative |= delta < 0;
    }
    EXPECT_TRUE(positive);
    EXPECT_TRUE(negative);
    ASSERT_EQ(a.end(), 0);
    ASSERT_EQ(b.end(), 0);
    sameBinary("network-quality-signed-0", "network-quality-signed-1", 5);
}

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, NetworkPollutantsRefuseBorrowedOrMissingRowPublisherAtomically) {
    SWMMEngine e;
    ASSERT_NO_THROW(startNetworkPollutants(e, "network-quality-publisher", 0, true, true, 0));
    auto& state = e.surfaceRouter2D().state();
    const auto* owned = state.node_row_conc;
    ASSERT_NE(owned, nullptr);
    auto borrowed = *owned;
    const auto before = root() / "network-quality-publisher-before.chk",
               after = root() / "network-quality-publisher-after.chk",
               bad = root() / "network-quality-publisher-bad.chk";
    ASSERT_EQ(e.saveCheckpoint(before.string()), "");
    for (const auto* replacement : {static_cast<const std::vector<double>*>(&borrowed),
                                    static_cast<const std::vector<double>*>(nullptr)}) {
        state.node_row_conc = replacement;
        EXPECT_FALSE(e.saveCheckpoint(bad.string()).empty());
        EXPECT_FALSE(std::filesystem::exists(bad));
        state.node_row_conc = owned;
        ASSERT_EQ(e.saveCheckpoint(after.string()), "");
        EXPECT_TRUE(bytes(before) == bytes(after));
    }
}
#endif
