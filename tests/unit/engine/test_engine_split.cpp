// SPDX-License-Identifier: Apache-2.0
// Independent native participants: only installed public C interfaces are used.
#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_split.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_checkpoint.h>
#include <openswmm/engine/openswmm_massbalance.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/engine/openswmm_gw2d.h>
#include <openswmm/engine/openswmm_surface_ownership.h>
#include <openswmm/engine/openswmm_infrastructure.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <array>
#include <set>

namespace {
namespace fs = std::filesystem;
struct Engine {
    SWMM_Engine value = swmm_engine_create();
    Engine() = default;
    Engine(const Engine&) = delete;
    ~Engine() { swmm_engine_close(value); swmm_engine_destroy(value); }
};
struct OwnedFrame {
    SWMM_SplitFrame value = nullptr;
    ~OwnedFrame() { swmm_split_frame_destroy(value); }
};
std::string bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
std::array<double, 10> balance(SWMM_Engine engine) {
    std::array<double, 10> b{};
    EXPECT_EQ(swmm_2d_get_mass_balance(engine, &b[0], &b[1], &b[2], &b[3], &b[4],
                                      &b[5], &b[6], &b[7], &b[8], &b[9]), SWMM_OK);
    return b;
}
class EngineSplit : public ::testing::Test {
protected:
    fs::path directory, model;
    std::string extra_rows, override_model;
    enum class OutfallDirection { None, Discharge, Withdraw, Gated };
    OutfallDirection outfall_direction = OutfallDirection::None;
    bool mixed_outfall_node = false;
    bool ordinary_sources = false;
    bool reviewed_sources = false;
    bool joint_richards = false;
    bool joint_aquifer_gain = false;
    bool classical_lids = false;
    bool classical_mesh = true;
    int classical_bottom = 2;
    bool classical_conduit = false;
    void classicalModel(bool reviewed=true, int closure=0, bool independent=false,
                        bool gaining=false, int bottom=2, bool mixed=false, bool links=false) {
        if (mixed) richardsModel(closure,independent,true);
        else richardsModel(closure,independent);
        classical_lids=true;
        classical_mesh=bottom == 2;
        classical_bottom=bottom;
        classical_conduit=links;
        joint_richards=mixed;
        const auto begin=override_model.find("[LID_RICHARDS]");
        const auto end=override_model.find("[COORDINATES]",begin);
        if (!mixed) override_model.erase(begin,end-begin);
        else {
            override_model += R"(
[LID_CONTROLS]
Ordinary NODE
Ordinary SURFACE 75 .1
Ordinary MEDIA 150 .45 .2 .08 5 10 75
Ordinary AGGREGATE 100 .4 100
Ordinary BOTTOM 0 .1
)";
            const std::string from="TANK Stack 1 BOTTOM MESH_AQUIFER";
            override_model.replace(override_model.find(from),from.size(),"TANK Ordinary 35 BOTTOM MESH_AQUIFER");
        }
        if (!reviewed) {
            reviewed_sources=false; ordinary_sources=true;
            const std::string owners="[2D_SURFACE_OWNERSHIP]\nA SUBCATCH UNIFORM\nB SUBCATCH UNIFORM\n";
            override_model.erase(override_model.find(owners),owners.size());
            // Legacy gages sample at t+1 s; hold real rain beyond that first sample.
            const std::string gage="R INTENSITY 0:00:01";
            override_model.replace(override_model.find(gage),gage.size(),"R INTENSITY 0:01");
            const std::string rain="Rain 10/12/2026 00:00:00 8\nRain 10/12/2026 00:00:04 0";
            override_model.replace(override_model.find(rain),rain.size(),
                "Rain 10/12/2026 00:00:00 60\nRain 10/12/2026 00:01:00 0");
            override_model += "\n[2D_AQUIFER_OPTIONS]\nGW_ET NONE\n[2D_OPTIONS]\nEVAPORATION CLIMATE\n";
        }
        if (!mixed) {
            const std::string from="SINK Stack 1 BOTTOM MESH_AQUIFER";
            override_model.replace(override_model.find(from),from.size(),
                bottom == 2 ? "SINK Stack 35 BOTTOM MESH_AQUIFER" :
                bottom == 1 ? "SINK Stack 35 BOTTOM SEALED" : "SINK Stack 35 BOTTOM CURRENT");
            const std::string bed="SINK 1 KC 3600 DC .1 AREA 1\n";
            if (bottom != 2) override_model.erase(override_model.find(bed),bed.size());
        }
        if (gaining) {
            const std::string from="SINK 0 .325 .024";
            override_model.replace(override_model.find(from),from.size(),"SINK -.2 .325 .024");
            override_model.replace(override_model.find("HG0 1"),5,"HG0 2");
        }
        if (links) override_model += "\n[2D_AQUIFER_OPTIONS]\nLINK_SEEPAGE TWO_WAY\n[LOSSES]\nC 0 0 0 NO 100\n[COORDINATES]\nDRAIN 1.8 .5\n";
        const std::string shape="FUNCTIONAL 0 0 10 0 0";
        for (size_t at=0; (at=override_model.find(shape,at)) != std::string::npos; at+=shape.size())
            override_model.replace(at,shape.size(),"FUNCTIONAL 0 0 10 0 1");
        const std::string bottom_row="Stack BOTTOM 0 0";
        override_model.replace(override_model.find(bottom_row),bottom_row.size(),bottom == 0 ? "Stack BOTTOM 360 .1" : "Stack BOTTOM 0 .1");
        if (mixed) override_model += "\n[INFLOWS]\nTANK FLOW TankFeed FLOW 1 1\n[TIMESERIES]\nTankFeed 10/12/2026 00:00:00 .0001\nTankFeed 10/12/2026 00:00:08 .0001\n";
        override_model += "\n[EVAPORATION]\nCONSTANT 48\n";
    }
    bool reviewed_returns = false, reviewed_mapped = false;
    int mapped_direction = 0;
    void reviewedOutfallModel(bool mapped, int closure=0, bool independent=false,
                              bool richards=false, int direction=1, bool ordinary=false, bool mixed=false) {
        if (richards) richardsModel(closure,independent);
        else reviewedModel(closure,independent);
        reviewed_returns=!mapped;
        reviewed_mapped=mapped || mixed;
        if (!richards) {
            const std::string storage="SINK 0 100 0";
            override_model.replace(override_model.find(storage),storage.size(),
                mapped ? (direction == 1 ? "SINK 0 100 1" : "SINK 0 100 0") : "SINK 0 100 .1");
            const std::string pipe="C CIRCULAR .025";
            override_model.replace(override_model.find(pipe),pipe.size(),"C CIRCULAR .1");
        }
        if (mapped) {
            mapped_direction=direction;
            const std::string point="0 SINK 0 1\n";
            if (!richards) override_model.erase(override_model.find(point),point.size());
            override_model += "\n[2D_VERTEX_NODE_MAP]\n0 DRAIN .7 1\n";
            if (direction == 0) {
                const std::string outfall="DRAIN 0 FREE NO";
                override_model.replace(override_model.find(outfall),outfall.size(),"DRAIN 0 FREE YES");
            }
        } else {
            const std::string outfall="DRAIN 0 FREE NO";
            override_model.replace(override_model.find(outfall),outfall.size(),ordinary ? "DRAIN 0 FREE NO CATCH" : "DRAIN 0 FREE NO A");
        }
        if (mapped || mixed) {
            const std::string cell="0 1 2 3 .03\n";
            override_model.replace(override_model.find(cell),cell.size(),"0 1 2 3 .03 .2\n");
        }
        if (mixed) override_model += R"(
[STORAGE]
TANK 0 2 0 FUNCTIONAL 0 0 10 0 0
[OUTFALLS]
MESH 0 FREE NO
[CONDUITS]
C2 TANK MESH 5 .013 0 0
[XSECTIONS]
C2 CIRCULAR .1 0 0 0 1
[2D_VERTEX_NODE_MAP]
0 MESH .7 1
)";
        if (ordinary) {
            override_model.insert(override_model.find("[SUBAREAS]"),"CATCH R SINK .0002 45 1 .5 0\n");
            override_model.insert(override_model.find("[INFILTRATION]"),"CATCH .015 .12 .03 .05 35 OUTLET\n");
            override_model.insert(override_model.find("[LID_CONTROLS]"),"CATCH 75 5 .3\n");
            override_model.insert(override_model.find("[2D_OPTIONS]"),
                "CATCH 10 10\nCATCH 12 10\nCATCH 12 11\nCATCH 10 11\n");
        }
    }
    void richardsModel(int closure=0, bool independent=false, bool competing=false) {
        reviewedModel(closure,independent);
        joint_richards=true;
        const std::string original="SINK 0 100 0";
        override_model.replace(override_model.find(original),original.size(),"SINK 0 .325 .024");
        override_model += R"(
[LID_CONTROLS]
Stack NODE
Stack SURFACE 75 .1
Stack MEDIA 150 .45 .2 .08 5 10 75
Stack AGGREGATE 100 .4 100
Stack BOTTOM 0 0
[LID_NODES]
SINK Stack 1 BOTTOM MESH_AQUIFER
[LID_RICHARDS]
Stack OPTIONS 4 1e-7 1e-5 30
Stack 2 .03 2 1.6 .5 .0001
Stack 3 .03 2 1.6 .5 .0001
[COORDINATES]
SINK .2 .5
[2D_AQUIFER_NODE]
SINK 1 KC 3600 DC .1 AREA 1
)";
        if (competing) override_model += R"(
[STORAGE]
TANK 0 .325 .024 FUNCTIONAL 0 0 10 0 0
[LID_NODES]
TANK Stack 1 BOTTOM MESH_AQUIFER
[COORDINATES]
TANK .3 .5
[2D_AQUIFER_NODE]
TANK 1 KC 3600 DC .1 AREA 1
[OUTFALLS]
DRAIN2 0 FREE NO
[CONDUITS]
C2 TANK DRAIN2 5 .013 0 0
[XSECTIONS]
C2 CIRCULAR .025 0 0 0 1
)";
    }
    void reviewedModel(int closure=0, bool independent=false, bool evaporation=false) {
        reviewed_sources=true;
        groundwater=true;
        override_model=bytes(fs::path(__FILE__).parent_path() / "data/component_profiles/managed_sources.inp");
        const std::string output="OUTPUT_FILE authored-managed.h5\n";
        override_model.erase(override_model.find(output),output.size());
        override_model += R"(
[OPTIONS]
THREADS 1
MINIMUM_STEP .001
[CONDUITS]
C SINK DRAIN 5 .013 0 0
[XSECTIONS]
C CIRCULAR .025 0 0 0 1
[2D_AQUIFER_OPTIONS]
M_LAYERS 4
CLOSURE )" + std::string(closure==0 ? "CLOSED_FORM" : closure==1 ? "ENSLAVED" : "SIGMA") +
            "\nGW_CLOCK " + (independent ? "INDEPENDENT" : "SHARED") + "\n";
        if (evaporation) override_model += "\n[EVAPORATION]\nCONSTANT 48\n";
    }
    void startReviewedReference(Engine& engine) {
        int objects=0,shares=0,cells=0,valid=0;
        char token[64]{},diagnostic[2048]{};
        ASSERT_EQ(swmm_surface_owner_preview(engine.value,nullptr,-1,nullptr,0,&objects,
            nullptr,0,&shares,nullptr,0,&cells,&valid,token,sizeof(token),diagnostic,sizeof(diagnostic),nullptr,nullptr),0);
        ASSERT_EQ(valid,1) << diagnostic;
        SWMM_CheckpointSessionOptions options{};
        options.struct_size=sizeof(options); options.schema_version=1;
        options.output=SWMM_CHECKPOINT_OUTPUT_COMBINED; options.ownership_token=token;
        ASSERT_EQ(swmm_checkpoint_start_session(engine.value,&options),0) << swmm_checkpoint_last_error_msg();
    }
    bool groundwater = false;
    int groundwater_node_sign = 0, groundwater_link_sign = 0;
    void groundwaterModel(int closure=0, bool independent=false, bool links=false,
                          bool losing=false, bool competing=false, bool oneWay=false) {
        groundwater = true;
        groundwater_node_sign = links ? 0 : losing ? -1 : 1;
        groundwater_link_sign = links ? (losing || oneWay ? 1 : -1) : 0;
        override_model = R"([OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
START_TIME 00:00:00
END_DATE 01/01/2026
END_TIME 00:01:00
REPORT_STEP 00:00:02
ROUTING_STEP .31
MINIMUM_STEP .001
VARIABLE_STEP .75
THREADS 1
IGNORE_QUALITY YES
[STORAGE]
J -5 6 )" + std::string(losing ? "3" : ".1") + R"( FUNCTIONAL 0 0 10 0 0 3 50 .3
[OUTFALLS]
O -5.05 FREE NO
[CONDUITS]
C J O 8 .013 0 0
[XSECTIONS]
C CIRCULAR .5 0 0 0 2
[INFLOWS]
J FLOW Inflow FLOW 1 1
[TIMESERIES]
Inflow 00:00 .05
Inflow 01:00 .05
[COORDINATES]
J 1 5
O 9 5
[2D_OPTIONS]
BACKEND CPU
MAX_TIMESTEP .2
RECONSTRUCTION_ORDER 1
LTS_TIERS 2
REPORT_2D YES
REPORT_2D_STEP 2
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
BED_HEAT_MODEL NONE
COUPLING_SYNC .7
[2D_VERTICES]
0 0 0
10 0 0
10 10 0
0 10 0
[2D_TRIANGLES]
0 1 2 .03 .1
0 2 3 .03 .05
[2D_INFILTRATION_DEFAULTS]
* CONSTANT 10 - - - -
[2D_AQUIFER_OPTIONS]
MODE MESH
SOIL_CHAR GARDNER
GW_ET NONE
NODE_ENROLMENT ROWS
M_LAYERS 4
CLOSURE )" + std::string(closure==0 ? "CLOSED_FORM" : closure==1 ? "ENSLAVED" : "SIGMA") +
            "\nGW_CLOCK " + (independent ? "INDEPENDENT" : "SHARED") +
            "\nLINK_SEEPAGE " + (links ? oneWay ? "ONE_WAY" : "TWO_WAY" : "NONE") + R"(
GW_BASE_STEP .2
GW_MAX_STEP 4
[2D_AQUIFER]
* 36 8 .45 .1 2 HG0 )" + std::string(losing ? "2" : "6") + "\n";
        override_model += R"(
[GW_TRANSPORT_OPTIONS]
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
CONDUCTION NO
THERMAL_MODEL WATER_ONLY
)";
        if (links) override_model += "\n[LOSSES]\nC 0 0 0 NO 100\n";
        else override_model += "\n[2D_AQUIFER_NODE]\nJ 1 KC 360 DC .1 AREA 10\n";
        if (competing) override_model += R"(
[OUTFALLS]
O2 -5.05 FREE NO
[COORDINATES]
O2 9 5
[CONDUITS]
C2 J O2 8 .013 0 0
[XSECTIONS]
C2 CIRCULAR .5 0 0 0 3
[LOSSES]
C2 0 0 0 NO 100
)";
    }
    void watershedModel(bool lid = false, bool mapped = false, bool cascade = false) {
        ordinary_sources = true;
        if (mapped) {
            outfallModel(OutfallDirection::Discharge);
            outfall_direction = OutfallDirection::None;
        } else {
            writeModel(true, .7, true, true);
            override_model = bytes(model);
        }
        const auto end = override_model.find("END_TIME ");
        override_model.replace(end, override_model.find('\n',end)-end,"END_TIME 00:03:00");
        extra_rows = R"(
[OPTIONS]
WET_STEP 00:00:07
DRY_STEP 00:00:13
[RAINGAGES]
G INTENSITY 0:01 1 TIMESERIES Rain
G2 INTENSITY 0:01 1 TIMESERIES Other
[SUBCATCHMENTS]
S G )" + std::string(cascade ? "T" : "J") + R"( .1 70 50 1 0
[SUBAREAS]
S .01 .1 .2 1 25 OUTLET
[INFILTRATION]
S 60 5 4 7 0
[TIMESERIES]
Rain 00:00 60
Rain 00:01 0
Rain 00:02 30
Rain 00:03 0
Other 00:00 0
Other 00:01 30
Other 00:02 0
Other 00:03 0
[EVAPORATION]
CONSTANT 5
[2D_OPTIONS]
EVAPORATION CLIMATE
[REPORT]
SUBCATCHMENTS ALL
)";
        if (cascade) extra_rows += R"(
[SUBCATCHMENTS]
T G2 J .02 50 10 1 0
[SUBAREAS]
T .01 .1 .2 1 25 OUTLET
[INFILTRATION]
T 60 5 4 7 0
)";
        if (lid) extra_rows += R"(
[LID_CONTROLS]
Barrel RB
Barrel STORAGE 300 .75 0 0
Barrel DRAIN 5 .5 0 0
[LID_USAGE]
S Barrel 2 10 0 50 50 0 * )" + std::string(cascade ? "T" : "J") + "\n";
    }
    void outfallModel(OutfallDirection direction, bool overlap = false, bool spread = false, bool mixed = false) {
        outfall_direction = direction;
        mixed_outfall_node = mixed;
        override_model = R"([OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
START_TIME 00:00:00
END_DATE 01/01/2026
END_TIME 00:00:20
REPORT_STEP 00:00:02
ROUTING_STEP .31
MINIMUM_STEP .001
VARIABLE_STEP .75
THREADS 1
IGNORE_QUALITY YES
[STORAGE]
J -2 )" + std::string(mixed ? "1" : "10") + " " +
            (direction == OutfallDirection::Discharge ? "5" : "0") + R"( FUNCTIONAL 10 0 0
[OUTFALLS]
O 0 FREE )" + (direction == OutfallDirection::Gated ? "YES" : "NO") + R"(
[CONDUITS]
C J O 5 .013 0 0
[XSECTIONS]
C CIRCULAR 1 0 0 0 2
[2D_OPTIONS]
BACKEND CPU
MAX_TIMESTEP .2
RECONSTRUCTION_ORDER 1
REPORT_2D YES
REPORT_2D_STEP 2
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
BED_HEAT_MODEL NONE
COUPLING_SYNC .7
[2D_VERTICES]
0 0 0
1 0 0
0 1 0
-1 0 0
0 -1 0
[2D_TRIANGLES]
0 1 2 .03 .2
0 3 4 .03 2
[2D_VERTEX_NODE_MAP]
0 O .7 1
)";
        if (overlap) override_model += R"(
[STORAGE]
J2 -2 10 0 FUNCTIONAL 10 0 0
[OUTFALLS]
O2 0 FREE NO
[CONDUITS]
C2 J2 O2 5 .013 0 0
[XSECTIONS]
C2 CIRCULAR 1 0 0 0 1
[2D_VERTEX_NODE_MAP]
0 O2 .7 1
)";
        if (spread) override_model += R"(
[2D_NODE_COUPLING]
O OUTFALL SPREAD=2 TAILWATER_RELAX=.8
)";
        if (mixed) {
            const std::string original = "C CIRCULAR 1 0 0 0 2";
            override_model.replace(override_model.find(original),original.size(),"C CIRCULAR .05 0 0 0 2");
            override_model += "\n[2D_TRIANGLE_NODE_MAP]\n0 J .6 .02\n";
        }
    }
    void SetUp() override {
        directory = fs::path(SPLIT_EVIDENCE) / ::testing::UnitTest::GetInstance()->current_test_info()->name();
        fs::create_directories(directory);
        model = directory / "model.inp";
        writeModel(false);
    }
    void writeModel(bool spill, double sync = .7, bool variable = false, bool dry = false) {
        std::ofstream out(model);
        out << R"([OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
START_TIME 00:00:00
END_DATE 01/01/2026
END_TIME )" << (spill ? "00:01:00" : "00:00:10") << R"(
REPORT_START_DATE 01/01/2026
REPORT_START_TIME 00:00:00
ROUTING_STEP .31
REPORT_STEP 00:00:02
IGNORE_QUALITY YES
ALLOW_PONDING YES
MIN_SURFAREA 1
MINIMUM_STEP .001
VARIABLE_STEP )" << (variable ? ".75" : "0") << R"(
[JUNCTIONS]
J 0 1 .1 2 10
[OUTFALLS]
O 0 FIXED )" << (spill ? "1.8" : "0") << R"( NO
[CONDUITS]
C J O )" << (variable ? "1" : "30") << R"( .013 0 0
[XSECTIONS]
C CIRCULAR 1 0 0 0 1
[COORDINATES]
J 2 2
O 12 2
[REPORT]
CONTINUITY YES
NODES ALL
LINKS ALL
[2D_OPTIONS]
BACKEND CPU
MAX_TIMESTEP .2
RECONSTRUCTION_ORDER 1
REPORT_2D YES
REPORT_2D_STEP 2
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
BED_HEAT_MODEL NONE
COUPLING_SYNC )" << sync << R"(
[2D_VERTICES]
0 0 1
10 0 1
10 10 1
0 10 1
[2D_TRIANGLES]
0 1 2 .03 )" << (dry ? "0" : ".2") << "\n0 2 3 .03 " << (dry ? "0" : ".1") << R"(
[2D_TRIANGLE_NODE_MAP]
0 J .6 .02
)";
        ASSERT_TRUE(bool(out));
    }
    void open(Engine& engine, const char* name, bool binary = true) {
        if (reviewed_sources && std::string_view(name)=="native") {
            fs::remove(directory / "native.rpt"); fs::remove(directory / "native.out");
        }
        const auto rpt = (directory / (std::string(name) + ".rpt")).string();
        const auto out = (directory / (std::string(name) + ".out")).string();
        ASSERT_EQ(swmm_engine_open(engine.value, model.string().c_str(), rpt.c_str(),
                                   binary ? out.c_str() : nullptr, nullptr), SWMM_OK)
            << swmm_get_last_error_msg(engine.value);
    }
    void results(Engine& engine, const char* name) {
        const auto path = directory / (std::string(name) + ".h5");
        fs::remove(path);
        ASSERT_EQ(swmm_2d_set_results_file(engine.value, path.string().c_str()), SWMM_OK)
            << swmm_get_last_error_msg(engine.value);
    }
    void init(Engine& engine) {
        ASSERT_EQ(swmm_engine_initialize(engine.value), SWMM_OK) << swmm_get_last_error_msg(engine.value);
    }
    struct Peer {
        SWMM_Engine surface = nullptr;
        std::ofstream trace;
        std::ofstream source_trace;
        std::ofstream return_trace;
        double returned_water=0;
        int source_preparations=0;
        int lid_trials=0, lid_accepts=0, replay_phase=0;
        bool source_pending=false, source_active=false;
        int conductance = 0, advances = 0, finishes = 0, preparations = 0;
        std::set<double> steps;
        bool pending = false;
        bool replay = false, alias = false, throws = false;
        static int exchange(SWMM_Engine network, int phase, SWMM_SplitFrame request,
                            SWMM_SplitFrame* reply, void* user) {
            auto& peer = *static_cast<Peer*>(user);
            if (peer.throws) throw 5;
            if (peer.alias) { *reply = request; return SWMM_OK; }
            uint64_t seq = 0;
            double start = 0, dt = 0, h = 0, d = 0, q = 0, direct = 0, remaining = 0;
            swmm_split_frame_time(request, &seq, &start, &dt);
            std::array<double,16> before{};
            if (phase == SWMM_SPLIT_LID_TRIAL)
                for (int i=0; i<16; ++i) EXPECT_EQ(swmm_gw2d_get_ledger(peer.surface,i,&before[i]),0);
            const auto rc = swmm_split_evaluate(peer.surface, phase, request, reply);
            if (rc) return rc;
            if (phase == SWMM_SPLIT_LID_TRIAL)
                for (int i=0; i<16; ++i) {
                    double after=0; EXPECT_EQ(swmm_gw2d_get_ledger(peer.surface,i,&after),0);
                    EXPECT_EQ(before[i],after) << "Trial must not publish groundwater water";
                }
            if (phase == SWMM_SPLIT_SOURCE_PREPARE) ++peer.source_preparations;
            if (phase == SWMM_SPLIT_ADVANCE && peer.return_trace.is_open())
                for (int sc=0; sc<swmm_subcatch_count(network); ++sc) {
                    double sent=-1,accepted=-1;
                    EXPECT_EQ(swmm_split_frame_return(request,sc,&sent),0);
                    EXPECT_EQ(swmm_split_frame_return(*reply,sc,&accepted),0);
                    EXPECT_EQ(sent,accepted);
                    peer.returned_water+=accepted;
                    peer.return_trace << seq << ',' << start << ',' << dt << ',' << sc << ',' << accepted << '\n';
                }
            if (phase == SWMM_SPLIT_LID_TRIAL) ++peer.lid_trials;
            if (phase == SWMM_SPLIT_LID_ACCEPT) ++peer.lid_accepts;
            if (peer.source_trace.is_open() && (phase == SWMM_SPLIT_SOURCE_PREPARE ||
                phase == SWMM_SPLIT_ADVANCE || phase == SWMM_SPLIT_FINISH))
                for (int n=0; n<swmm_node_count(network); ++n) {
                    double rate=0,previous=0,runoff=0,drain=0;
                    EXPECT_EQ(swmm_split_frame_source(*reply,n,&rate,&previous,&runoff,&drain),0);
                    peer.source_pending |= runoff+drain > 0;
                    peer.source_active |= rate > 0;
                    peer.source_trace << phase << ',' << seq << ',' << n << ',' << start << ',' << dt
                        << ',' << rate << ',' << previous << ',' << runoff << ',' << drain << '\n';
                }
            swmm_split_frame_node(*reply, 0, &h, &d, &q, &direct, &remaining);
            peer.trace << phase << ',' << seq << ',' << start << ',' << dt << ',' << h << ','
                       << d << ',' << q << ',' << direct << ',' << remaining << '\n';
            peer.pending |= q != 0 || direct != 0;
            if (phase == SWMM_SPLIT_OUTFALL_PREPARE) ++peer.preparations;
            if (phase == SWMM_SPLIT_CONDUCTANCE) ++peer.conductance;
            if (phase == SWMM_SPLIT_ADVANCE) { ++peer.advances; peer.steps.insert(dt); }
            if (phase == SWMM_SPLIT_FINISH) ++peer.finishes;
            if (peer.replay && (!peer.replay_phase || peer.replay_phase == phase)) {
                // A successful receipt cannot be used a second time, even without a clock advance.
                SWMM_SplitFrame sentinel = *reply;
                EXPECT_NE(swmm_split_evaluate(peer.surface, phase, request, &sentinel), SWMM_OK);
                EXPECT_EQ(sentinel, *reply);
                return SWMM_ERR_BADPARAM;
            }
            return SWMM_OK;
        }
    };
    void richardsReplay(int phase) {
        richardsModel(); std::ofstream(model) << override_model;
        Engine network,surface;
        ASSERT_NO_FATAL_FAILURE(open(network,"network"));
        ASSERT_NO_FATAL_FAILURE(open(surface,"surface",false));
        ASSERT_EQ(swmm_split_configure(network.value,SWMM_SPLIT_NETWORK),0);
        ASSERT_EQ(swmm_split_configure(surface.value,SWMM_SPLIT_SURFACE),0);
        ASSERT_NO_FATAL_FAILURE(results(surface,"surface"));
        ASSERT_NO_FATAL_FAILURE(init(surface));
        OwnedFrame geometry;
        ASSERT_EQ(swmm_split_geometry(surface.value,&geometry.value),0);
        ASSERT_NO_FATAL_FAILURE(init(network));
        ASSERT_EQ(swmm_split_apply_geometry(network.value,geometry.value),0);
        Peer peer; peer.surface=surface.value; peer.replay=true; peer.replay_phase=phase;
        ASSERT_EQ(swmm_split_set_callback(network.value,Peer::exchange,&peer),0);
        ASSERT_EQ(swmm_engine_start(surface.value,0),0);
        ASSERT_EQ(swmm_engine_start(network.value,1),0);
        double elapsed=0,time=-1;
        EXPECT_NE(swmm_engine_step(network.value,&elapsed),0);
        EXPECT_EQ(peer.lid_trials,1); EXPECT_EQ(peer.lid_accepts,phase == SWMM_SPLIT_LID_ACCEPT ? 1 : 0);
        EXPECT_EQ(peer.advances,0);
        ASSERT_EQ(swmm_engine_get_elapsed_seconds(surface.value,&time),0); EXPECT_EQ(time,0);
        ASSERT_EQ(swmm_engine_get_elapsed_seconds(network.value,&time),0); EXPECT_EQ(time,0);
    }
    void connect(Engine& network, Engine& surface, Peer& peer) {
        ASSERT_EQ(swmm_split_configure(network.value, SWMM_SPLIT_NETWORK), SWMM_OK)
            << swmm_get_last_error_msg(network.value);
        ASSERT_EQ(swmm_split_configure(surface.value, SWMM_SPLIT_SURFACE), SWMM_OK)
            << swmm_get_last_error_msg(surface.value);
        EXPECT_EQ(swmm_2d_set_results_file(network.value, (directory / "forbidden.h5").string().c_str()), SWMM_ERR_LIFECYCLE);
        ASSERT_NO_FATAL_FAILURE(init(surface));
        OwnedFrame geometry;
        ASSERT_EQ(swmm_split_geometry(surface.value, &geometry.value), SWMM_OK)
            << swmm_get_last_error_msg(surface.value);
        ASSERT_NO_FATAL_FAILURE(init(network));
        ASSERT_EQ(swmm_split_apply_geometry(network.value, geometry.value), SWMM_OK)
            << swmm_get_last_error_msg(network.value);
        peer.surface = surface.value;
        peer.trace.open(directory / "phases.csv");
        peer.trace.precision(17);
        peer.trace << "phase,sequence,start_s,dt_s,head_m,depth_m,queued_m3,direct_m3,remaining_s\n";
        ASSERT_EQ(swmm_split_set_callback(network.value, Peer::exchange, &peer), SWMM_OK);
    }
    void parity(bool spill, double sync = .7, bool variable = false, bool dry = false,
                bool meshOutput = true, int stopAfter = 10000) {
        if (override_model.empty()) writeModel(spill, sync, variable, dry);
        else std::ofstream(model) << override_model;
        std::ofstream(model, std::ios::app) << extra_rows;
        Engine reference, network, surface;
        ASSERT_NO_FATAL_FAILURE(open(reference, "native"));
        ASSERT_NO_FATAL_FAILURE(open(network, "network"));
        ASSERT_NO_FATAL_FAILURE(open(surface, "surface", false));
        // Configure roles before private output binding, initialize only afterward.
        ASSERT_EQ(swmm_split_configure(network.value, SWMM_SPLIT_NETWORK), SWMM_OK)
            << swmm_get_last_error_msg(network.value);
        ASSERT_EQ(swmm_split_configure(surface.value, SWMM_SPLIT_SURFACE), SWMM_OK)
            << swmm_get_last_error_msg(surface.value);
        if (meshOutput) {
            ASSERT_NO_FATAL_FAILURE(results(reference, "native"));
            ASSERT_NO_FATAL_FAILURE(results(surface, "surface"));
        }
        if (!reviewed_sources) ASSERT_NO_FATAL_FAILURE(init(reference));
        ASSERT_NO_FATAL_FAILURE(init(surface));
        OwnedFrame geometry;
        ASSERT_EQ(swmm_split_geometry(surface.value, &geometry.value), SWMM_OK)
            << swmm_get_last_error_msg(surface.value);
        ASSERT_NO_FATAL_FAILURE(init(network));
        ASSERT_EQ(swmm_split_apply_geometry(network.value, geometry.value), SWMM_OK);
        Peer peer;
        peer.surface = surface.value;
        peer.trace.open(directory / "phases.csv"); peer.trace.precision(17);
        if (reviewed_sources) {
            peer.source_trace.open(directory / "source-offers.csv");
            peer.source_trace.precision(17);
            peer.source_trace << "phase,sequence,node,start_s,duration_s,rate_cfs,previous_cfs,pending_runoff_ft3,pending_drain_ft3\n";
        }
        if (reviewed_returns) {
            peer.return_trace.open(directory/"outfall-returns.csv"); peer.return_trace.precision(17);
            peer.return_trace << "sequence,start_s,duration_s,subcatchment,accepted_ft3\n";
        }
        peer.trace << "phase,sequence,start_s,dt_s,head_m,depth_m,queued_m3,direct_m3,remaining_s\n";
        ASSERT_EQ(swmm_split_set_callback(network.value, Peer::exchange, &peer), SWMM_OK);
        int active = -1;
        ASSERT_EQ(swmm_2d_is_active(network.value, &active), SWMM_OK); EXPECT_EQ(active, 0);
        ASSERT_EQ(swmm_2d_is_active(surface.value, &active), SWMM_OK); EXPECT_EQ(active, 1);
        ASSERT_EQ(swmm_engine_start(surface.value, 0), SWMM_OK) << swmm_get_last_error_msg(surface.value);
        ASSERT_EQ(swmm_engine_start(network.value, 1), SWMM_OK) << swmm_get_last_error_msg(network.value);
        if (reviewed_sources) ASSERT_NO_FATAL_FAILURE(startReviewedReference(reference));
        else ASSERT_EQ(swmm_engine_start(reference.value, 1), SWMM_OK);
        double before = 0, after = 0, elapsed = 123;
        swmm_get_current_time(surface.value, &before);
        EXPECT_EQ(swmm_engine_step(surface.value, &elapsed), SWMM_ERR_LIFECYCLE);
        double actual = -1;
        EXPECT_EQ(swmm_engine_advance_to(surface.value, 1., &actual), SWMM_ERR_LIFECYCLE);
        EXPECT_EQ(swmm_engine_advance_to(network.value, 1., &actual), SWMM_ERR_LIFECYCLE);
        swmm_get_current_time(surface.value, &after); EXPECT_EQ(before, after);
        std::ofstream ledger(directory / "native-ledger.txt");
        ledger.precision(17);
        std::ofstream source_ledger;
        std::ofstream groundwater_ledger;
        std::ofstream richards_ledger;
        std::ofstream classical_ledger;
        if (classical_lids) {
            classical_ledger.open(directory/"native-lid-moisture.csv"); classical_ledger.precision(17);
            classical_ledger << "phase,node,cell,layer,bottom,top,moisture\n";
        }
        if (joint_richards) {
            richards_ledger.open(directory/"native-richards-ledger.csv"); richards_ledger.precision(17);
            richards_ledger << "phase,node,down_m3,up_m3,pending_down_m3,pending_up_m3,requested_down_m3,requested_up_m3,reserved_down_m3,reserved_up_m3,accepted_down_m3,accepted_up_m3,aquifer_time_s,start_s,end_s,accepted,rejected,rhs,newton,min_step_s,balance_m3\n";
        }
        if (groundwater) {
            groundwater_ledger.open(directory / "native-groundwater-ledger.txt");
            groundwater_ledger.precision(17);
            int cells=-1;
            EXPECT_EQ(swmm_gw2d_get_dimensions(network.value,&cells,nullptr),SWMM_ERR_LIFECYCLE);
            EXPECT_EQ(cells,-1) << "Network must not own a duplicate aquifer";
        }
        if (ordinary_sources || reviewed_sources) {
            source_ledger.open(directory / "native-source-ledger.txt");
            source_ledger.precision(17);
        }
        auto publish = [&](int phase) {
            if (classical_lids) {
                for (int n=0; n<swmm_node_count(reference.value); ++n) {
                    const int count=swmm_lid_node_state_count(reference.value,n);
                    if (count <= 0) continue;
                    for (int row=0; row<count; ++row) {
                        int layer=0; double bottom=0,top=0,moisture=0;
                        ASSERT_EQ(swmm_lid_node_state_get(reference.value,n,row,&layer,&bottom,&top,&moisture),0);
                        classical_ledger << phase << ',' << n << ',' << row << ',' << layer << ','
                            << bottom << ',' << top << ',' << moisture << '\n';
                        for (auto target : {network.value,surface.value}) {
                            int l=0; double b=0,t=0,m=0;
                            ASSERT_EQ(swmm_lid_node_state_get(target,n,row,&l,&b,&t,&m),0);
                            EXPECT_EQ(layer,l); EXPECT_EQ(bottom,b); EXPECT_EQ(top,t); EXPECT_EQ(moisture,m);
                        }
                    }
                    if (classical_mesh) {
                        SWMM_LidBottomState a{},b{};
                        ASSERT_EQ(swmm_lid_node_bottom_state_get(reference.value,n,&a),0);
                        ASSERT_EQ(swmm_lid_node_bottom_state_get(surface.value,n,&b),0);
                        EXPECT_EQ(a.flow_model,b.flow_model); EXPECT_EQ(a.cell,b.cell);
                        EXPECT_EQ(a.down_cumulative_m3,b.down_cumulative_m3);
                        EXPECT_EQ(a.up_cumulative_m3,b.up_cumulative_m3);
                        EXPECT_EQ(a.pending_down_m3,b.pending_down_m3); EXPECT_EQ(a.pending_up_m3,b.pending_up_m3);
                        EXPECT_EQ(a.aquifer_time_s,b.aquifer_time_s);
                        if (phase == SWMM_SPLIT_FINISH && stopAfter > 1)
                            EXPECT_GT(a.down_cumulative_m3+a.up_cumulative_m3,0);
                    }
                }
            }
            if (joint_richards) {
                for (int n=0; n<swmm_node_count(reference.value); ++n) {
                    const int count=swmm_lid_node_state_count(reference.value,n);
                    if (count <= 0) continue;
                    SWMM_LidRichardsStatistics probe{};
                    if (swmm_lid_richards_statistics_get(reference.value,n,&probe) != 0) continue;
                    for (int row=0; row<count; ++row) {
                        double pressure=0,head=0,water=0;
                        ASSERT_EQ(swmm_lid_richards_state_get(reference.value,n,row,&pressure,&head,&water),0);
                        for (auto target : {network.value,surface.value}) {
                            double p=0,h=0,w=0;
                            ASSERT_EQ(swmm_lid_richards_state_get(target,n,row,&p,&h,&w),0);
                            EXPECT_EQ(pressure,p); EXPECT_EQ(head,h); EXPECT_EQ(water,w);
                        }
                    }
                    SWMM_LidBottomState a{},b{};
                    ASSERT_EQ(swmm_lid_node_bottom_state_get(reference.value,n,&a),0);
                    ASSERT_EQ(swmm_lid_node_bottom_state_get(surface.value,n,&b),0);
#define LID_FIELD(field) EXPECT_EQ(a.field,b.field) << #field
                    LID_FIELD(down_cumulative_m3); LID_FIELD(up_cumulative_m3);
                    LID_FIELD(pending_down_m3); LID_FIELD(pending_up_m3);
                    LID_FIELD(requested_down_m3); LID_FIELD(requested_up_m3);
                    LID_FIELD(reserved_down_m3); LID_FIELD(reserved_up_m3);
                    LID_FIELD(accepted_down_m3); LID_FIELD(accepted_up_m3);
                    LID_FIELD(aquifer_time_s); LID_FIELD(sample_start_s); LID_FIELD(sample_end_s);
                    LID_FIELD(contact_area_m2); LID_FIELD(surface_head_m); LID_FIELD(aquifer_head_m);
                    LID_FIELD(net_cumulative_m3); LID_FIELD(groundwater_residual_m3);
                    LID_FIELD(sampled_aquifer_time_s); LID_FIELD(sampled_lid_head_m);
                    LID_FIELD(sampled_aquifer_head_m); LID_FIELD(net_rate_m3s);
#undef LID_FIELD
                    SWMM_LidRichardsStatistics aStats{},bStats{};
                    ASSERT_EQ(swmm_lid_richards_statistics_get(reference.value,n,&aStats),0);
                    ASSERT_EQ(swmm_lid_richards_statistics_get(network.value,n,&bStats),0);
                    EXPECT_EQ(aStats.accepted,bStats.accepted); EXPECT_EQ(aStats.rejected,bStats.rejected);
                    EXPECT_EQ(aStats.rhs,bStats.rhs); EXPECT_EQ(aStats.newton,bStats.newton);
                    EXPECT_EQ(aStats.min_step,bStats.min_step); EXPECT_EQ(aStats.balance_m3,bStats.balance_m3);
                    EXPECT_GT(aStats.accepted,0);
                    if (phase == SWMM_SPLIT_FINISH)
                        EXPECT_GT(a.down_cumulative_m3+a.up_cumulative_m3,0);
                    if (phase == SWMM_SPLIT_FINISH && joint_aquifer_gain) EXPECT_GT(a.up_cumulative_m3,0);
                    richards_ledger << phase << ',' << n << ',' << a.down_cumulative_m3 << ',' << a.up_cumulative_m3
                        << ',' << a.pending_down_m3 << ',' << a.pending_up_m3 << ',' << a.requested_down_m3
                        << ',' << a.requested_up_m3 << ',' << a.reserved_down_m3 << ',' << a.reserved_up_m3
                        << ',' << a.accepted_down_m3 << ',' << a.accepted_up_m3 << ',' << a.aquifer_time_s
                        << ',' << a.sample_start_s << ',' << a.sample_end_s << ',' << aStats.accepted
                        << ',' << aStats.rejected << ',' << aStats.rhs << ',' << aStats.newton
                        << ',' << aStats.min_step << ',' << aStats.balance_m3 << '\n';
                }
            }
            ledger << phase;
            for (double term : balance(reference.value)) ledger << ' ' << term;
            ledger << '\n';
            if (groundwater) {
                groundwater_ledger << phase;
                for (int term=0; term<16; ++term) {
                    double expected=0, actual=0;
                    ASSERT_EQ(swmm_gw2d_get_ledger(reference.value,term,&expected),0);
                    ASSERT_EQ(swmm_gw2d_get_ledger(surface.value,term,&actual),0);
                    EXPECT_EQ(expected,actual) << "aquifer ledger " << term << " phase " << phase;
                    groundwater_ledger << ' ' << expected;
                }
                groundwater_ledger << '\n';
                int cells=0, layers=0;
                ASSERT_EQ(swmm_gw2d_get_dimensions(reference.value,&cells,&layers),0);
                for (int cell=0; cell<cells; ++cell) {
                    for (int var : {0,1,5,11,12,13,17,30,31,32}) {
                        double expected=0, actual=0;
                        ASSERT_EQ(swmm_gw2d_get_cell(reference.value,cell,var,&expected),0);
                        ASSERT_EQ(swmm_gw2d_get_cell(surface.value,cell,var,&actual),0);
                        EXPECT_EQ(expected,actual) << "aquifer cell " << cell << " variable " << var;
                    }
                    double closure=0;
                    ASSERT_EQ(swmm_gw2d_get_cell(reference.value,cell,SWMM_GW2D_VAR_CLOSURE,&closure),0);
                    if (closure == 2) {
                        std::vector<double> a(layers), b(layers);
                        int na=0,nb=0;
                        ASSERT_EQ(swmm_gw2d_get_column(reference.value,cell,a.data(),layers,&na),0);
                        ASSERT_EQ(swmm_gw2d_get_column(surface.value,cell,b.data(),layers,&nb),0);
                        EXPECT_EQ(na,nb); EXPECT_EQ(a,b);
                    }
                }
            }
            if (ordinary_sources || reviewed_sources) {
                source_ledger << phase;
                for (int term=0; term<=8; ++term) {
                    double expected=0, actual=0;
                    ASSERT_EQ(swmm_get_runoff_total(reference.value,term,&expected),0);
                    ASSERT_EQ(swmm_get_runoff_total(network.value,term,&actual),0);
                    EXPECT_EQ(expected,actual) << "runoff ledger " << term << " phase " << phase;
                    source_ledger << ' ' << expected;
                }
                source_ledger << '\n';
            }
        };
        int steps = 0;
        for (; steps < stopAfter; ++steps) {
            double a = 0, b = 0;
            ASSERT_EQ(swmm_engine_step(reference.value, &a), SWMM_OK) << swmm_get_last_error_msg(reference.value);
            ASSERT_EQ(swmm_engine_step(network.value, &b), SWMM_OK) << swmm_get_last_error_msg(network.value)
                << "; surface: " << swmm_get_last_error_msg(surface.value);
            ASSERT_EQ(a, b) << "step " << steps;
            EXPECT_EQ(balance(reference.value), balance(surface.value)) << "step " << steps;
            for (int term=0; term<=13; ++term) {
                double expected=0, actual=0;
                ASSERT_EQ(swmm_get_routing_total(reference.value,term,&expected),SWMM_OK);
                ASSERT_EQ(swmm_get_routing_total(network.value,term,&actual),SWMM_OK);
                EXPECT_EQ(expected,actual) << "1D water ledger term " << term << " step " << steps;
            }
            if (ordinary_sources) {
                double duplicated=0;
                for (int term=0; term<=4; ++term) {
                    ASSERT_EQ(swmm_get_runoff_total(surface.value,term,&duplicated),0);
                    EXPECT_EQ(duplicated,0) << "Surface must not advance a second watershed/LID";
                }
                for (int sc=0; sc<swmm_subcatch_count(surface.value); ++sc) {
                    ASSERT_EQ(swmm_subcatch_get_runoff(surface.value,sc,&duplicated),0);
                    EXPECT_EQ(duplicated,0);
                }
            }
            if (a == 0) break;
            publish(SWMM_SPLIT_ADVANCE);
        }
        ASSERT_LT(steps, 10000);
        ASSERT_EQ(swmm_engine_end(reference.value), SWMM_OK);
        ASSERT_EQ(swmm_engine_end(network.value), SWMM_OK) << swmm_get_last_error_msg(network.value)
            << "; surface: " << swmm_get_last_error_msg(surface.value);
        EXPECT_EQ(balance(reference.value), balance(surface.value)) << "Finalized surface ledger";
        publish(SWMM_SPLIT_FINISH);
        double nativeIn = 0, nativeOut = 0, splitIn = 0, splitOut = 0;
        ASSERT_EQ(swmm_2d_get_mass_balance(reference.value, nullptr, nullptr, nullptr,
            &nativeIn, &nativeOut, nullptr, nullptr, nullptr, nullptr, nullptr), SWMM_OK);
        ASSERT_EQ(swmm_2d_get_mass_balance(surface.value, nullptr, nullptr, nullptr,
            &splitIn, &splitOut, nullptr, nullptr, nullptr, nullptr, nullptr), SWMM_OK);
        EXPECT_EQ(nativeIn, splitIn); EXPECT_EQ(nativeOut, splitOut);
        const auto referenceBalance = balance(reference.value);
        if (classical_lids && stopAfter > 1) {
            double evaporation=0,exfiltration=0,link=0;
            ASSERT_EQ(swmm_get_routing_total(reference.value,SWMM_ROUTING_EVAP_LOSS,&evaporation),0);
            ASSERT_EQ(swmm_get_routing_total(reference.value,SWMM_ROUTING_SEEP_LOSS,&exfiltration),0);
            EXPECT_GT(evaporation,0);
            if (classical_bottom == 0) EXPECT_GT(exfiltration,0);
            if (classical_conduit) {
                ASSERT_EQ(swmm_gw2d_get_ledger(reference.value,SWMM_GW2D_LED_LINK,&link),0);
                EXPECT_NE(link,0);
            }
            std::ofstream(directory/"classical-activity.json") << std::setprecision(17)
                << "{\"evaporation_ft3\":" << evaporation << ",\"exfiltration_ft3\":" << exfiltration
                << ",\"conduit_exchange_m3\":" << link << "}\n";
        }
        if (outfall_direction != OutfallDirection::None) {
            EXPECT_EQ(peer.preparations,steps);
            if (outfall_direction == OutfallDirection::Discharge) {
                EXPECT_GT(referenceBalance[5],0);
                EXPECT_GT(referenceBalance[6],0) << "Discharge scenario must also reverse";
            }
            else if (outfall_direction == OutfallDirection::Withdraw) EXPECT_GT(referenceBalance[6],0);
            else EXPECT_EQ(referenceBalance[6],0);
            if (mixed_outfall_node) EXPECT_GT(referenceBalance[4],1e-6);
        } else if (ordinary_sources) {
            double rainfall=0, wet=0;
            ASSERT_EQ(swmm_get_runoff_total(network.value,SWMM_RUNOFF_RAINFALL,&rainfall),0);
            ASSERT_EQ(swmm_get_routing_total(network.value,SWMM_ROUTING_WET_WEATHER,&wet),0);
            EXPECT_GT(rainfall,0);
            if (stopAfter > 1) {
                EXPECT_GT(wet,0);
                EXPECT_GT(referenceBalance[2],0);
                EXPECT_GT(referenceBalance[9],0);
            }
        } else if (reviewed_sources) {
            double rain=0,wet=0,infil=0;
            ASSERT_EQ(swmm_get_runoff_total(network.value,SWMM_RUNOFF_RAINFALL,&rain),0);
            ASSERT_EQ(swmm_get_routing_total(network.value,SWMM_ROUTING_WET_WEATHER,&wet),0);
            ASSERT_EQ(swmm_gw2d_get_ledger(surface.value,SWMM_GW2D_LED_INFIL_IN,&infil),0);
            if (stopAfter > 1) {
                EXPECT_GT(rain,0); EXPECT_GT(wet,0); EXPECT_GT(infil,0);
                EXPECT_TRUE(peer.source_pending); EXPECT_TRUE(peer.source_active);
            }
            EXPECT_EQ(peer.source_preparations,steps);
        } else if (groundwater) {
            double node=0,link=0;
            ASSERT_EQ(swmm_gw2d_get_ledger(reference.value,SWMM_GW2D_LED_NODE,&node),0);
            ASSERT_EQ(swmm_gw2d_get_ledger(reference.value,SWMM_GW2D_LED_LINK,&link),0);
            if (stopAfter > 1) {
                if (groundwater_node_sign) EXPECT_GT(node * groundwater_node_sign,0);
                if (groundwater_link_sign) EXPECT_GT(link * groundwater_link_sign,0);
            }
            std::ofstream(directory / "groundwater-exchange.json") << std::setprecision(17)
                << "{\"node_m3\":" << node << ",\"link_m3\":" << link << "}\n";
        } else if (spill) EXPECT_GT(nativeIn, 0);
        else if (!dry) EXPECT_GT(nativeOut, 0);
        std::ofstream(directory / "exchange.json") << std::setprecision(17)
            << "{\"native_spill_m3\":" << nativeIn << ",\"native_drain_m3\":" << nativeOut
            << ",\"split_spill_m3\":" << splitIn << ",\"split_drain_m3\":" << splitOut << "}\n";
        EXPECT_GT(peer.conductance, steps); EXPECT_EQ(peer.advances, steps); EXPECT_EQ(peer.finishes, 1);
        if (joint_richards) { EXPECT_EQ(peer.lid_trials,steps); EXPECT_EQ(peer.lid_accepts,steps); }
        if (reviewed_mapped) {
            EXPECT_EQ(peer.preparations,steps);
            if (mapped_direction == 1) EXPECT_GT(referenceBalance[5],0);
            if (mapped_direction == -1) EXPECT_GT(referenceBalance[6],0);
            if (mapped_direction == 0) EXPECT_EQ(referenceBalance[6],0);
        }
        if (reviewed_returns && swmm_subcatch_count(reference.value) == 2) EXPECT_GT(peer.returned_water,0);
        if (variable) EXPECT_GT(peer.steps.size(), 2u) << "Adaptive routing must actually change its step";
        if (!dry && !groundwater && !ordinary_sources && outfall_direction == OutfallDirection::None) EXPECT_TRUE(peer.pending) << "The test must exercise actual exchange";
        ASSERT_EQ(swmm_engine_report(reference.value), SWMM_OK);
        ASSERT_EQ(swmm_engine_report(network.value), SWMM_OK);
        ASSERT_EQ(swmm_engine_report(surface.value), SWMM_OK);
        ASSERT_EQ(swmm_engine_close(reference.value), SWMM_OK);
        ASSERT_EQ(swmm_engine_close(network.value), SWMM_OK);
        ASSERT_EQ(swmm_engine_close(surface.value), SWMM_OK);
        EXPECT_FALSE(fs::exists(directory / "surface.out"));
        ASSERT_FALSE(bytes(directory / "native.out").empty());
        EXPECT_EQ(bytes(directory / "native.out"), bytes(directory / "network.out"));
        // Complete HDF5 content and attribute equality is a separate CTest fixture.
    }
};
TEST_F(EngineSplit, DrainingSurface) { parity(false); }
TEST_F(EngineSplit, SpillingNetwork) { parity(true); }
TEST_F(EngineSplit, PerStepExchange) { parity(false, 0); }
TEST_F(EngineSplit, VariableRouting) { parity(true, .7, true); }
TEST_F(EngineSplit, DrySurface) { parity(false, .7, false, true); }
TEST_F(EngineSplit, BinaryOnlyRetainsReportFlushes) { parity(false, .7, false, false, false); }
TEST_F(EngineSplit, EarlyFinishFlushesPendingBatch) { parity(false, .7, false, false, true, 1); }
TEST_F(EngineSplit, CompetingWithdrawals) {
    extra_rows = R"(
[JUNCTIONS]
J2 0 1 .05 2 10
[OUTFALLS]
O2 0 FIXED 0 NO
[CONDUITS]
C2 J2 O2 30 .013 0 0
[XSECTIONS]
C2 CIRCULAR 1 0 0 0 1
[2D_TRIANGLE_NODE_MAP]
0 J .6 10
0 J2 .6 10
)";
    parity(false);
}
TEST_F(EngineSplit, OutfallDischarge) {
    outfallModel(OutfallDirection::Discharge); parity(false);
}
TEST_F(EngineSplit, OutfallWithdrawal) {
    outfallModel(OutfallDirection::Withdraw); parity(false);
}
TEST_F(EngineSplit, OutfallFlapGate) {
    outfallModel(OutfallDirection::Gated); parity(false);
}
TEST_F(EngineSplit, OutfallOverlap) {
    outfallModel(OutfallDirection::Withdraw,true); parity(false);
}
TEST_F(EngineSplit, OutfallSpreadRelaxation) {
    outfallModel(OutfallDirection::Discharge,false,true); parity(false);
}
TEST_F(EngineSplit, OutfallMixedNodeExchange) {
    outfallModel(OutfallDirection::Withdraw,false,false,true); parity(false);
}
TEST_F(EngineSplit, OutfallBinaryOnly) {
    outfallModel(OutfallDirection::Discharge); parity(false,.7,false,false,false);
}
TEST_F(EngineSplit, OutfallEarlyFinish) {
    outfallModel(OutfallDirection::Withdraw); parity(false,.7,false,false,true,1);
}
TEST_F(EngineSplit, WatershedRainfall) { watershedModel(); parity(false); }
TEST_F(EngineSplit, WatershedOutfall) { watershedModel(false,true); parity(false); }
TEST_F(EngineSplit, ConventionalLidOutlets) { watershedModel(true); parity(false); }
TEST_F(EngineSplit, ConventionalLidOverflow) {
    watershedModel(true);
    const auto pos=extra_rows.find("STORAGE 300");
    ASSERT_NE(pos,std::string::npos);
    extra_rows.replace(pos,11,"STORAGE 5");
    parity(false);
}
TEST_F(EngineSplit, ConventionalLidCascade) { watershedModel(true,true,true); parity(false); }
TEST_F(EngineSplit, WatershedBinaryOnly) { watershedModel(true); parity(false,.7,false,false,false); }
TEST_F(EngineSplit, WatershedEarlyFinish) { watershedModel(true); parity(false,.7,false,false,true,1); }
TEST_F(EngineSplit, GroundwaterNodeGain) { groundwaterModel(); parity(false); }
TEST_F(EngineSplit, GroundwaterNodeLoss) { groundwaterModel(0,false,false,true); parity(false); }
TEST_F(EngineSplit, GroundwaterIndependent) { groundwaterModel(0,true); parity(false); }
TEST_F(EngineSplit, GroundwaterEnslaved) { groundwaterModel(1,true); parity(false); }
TEST_F(EngineSplit, GroundwaterSigma) { groundwaterModel(2,true); parity(false); }
TEST_F(EngineSplit, GroundwaterLinkGain) { groundwaterModel(0,true,true); parity(false); }
TEST_F(EngineSplit, GroundwaterLinkLoss) { groundwaterModel(0,true,true,true); parity(false); }
TEST_F(EngineSplit, GroundwaterCompetingLinks) { groundwaterModel(0,false,true,false,true); parity(false); }
TEST_F(EngineSplit, GroundwaterOneWayLoss) { groundwaterModel(0,true,true,false,false,true); parity(false); }
TEST_F(EngineSplit, GroundwaterUSUnits) {
    groundwaterModel(0,true,true);
    const auto pos=override_model.find("FLOW_UNITS CMS");
    ASSERT_NE(pos,std::string::npos);
    override_model.replace(pos,14,"FLOW_UNITS CFS");
    parity(false);
}
TEST_F(EngineSplit, GroundwaterBinaryOnly) { groundwaterModel(0,true,true); parity(false,.7,false,false,false); }
TEST_F(EngineSplit, GroundwaterEarlyFinish) { groundwaterModel(0,true,true); parity(false,.7,false,false,true,1); }
TEST_F(EngineSplit, GroundwaterWatershed) {
    watershedModel(true,true,true);
    groundwater=true;
    extra_rows += R"(
[2D_AQUIFER_OPTIONS]
MODE MESH
SOIL_CHAR GARDNER
CLOSURE CLOSED_FORM
GW_ET NONE
NODE_ENROLMENT ROWS
GW_CLOCK INDEPENDENT
GW_BASE_STEP .2
GW_MAX_STEP 4
LINK_SEEPAGE TWO_WAY
[2D_AQUIFER]
* 36 8 .45 .1 2 HG0 6
[2D_AQUIFER_NODE]
J 1 KC 360 DC .1 AREA 1
[GW_TRANSPORT_OPTIONS]
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
CONDUCTION NO
THERMAL_MODEL WATER_ONLY
)";
    parity(false);
}
TEST_F(EngineSplit, ReviewedFootprintSources) { reviewedModel(); parity(false); }
TEST_F(EngineSplit, ClassicalMeshReviewedShared) { classicalModel(); parity(false); }
TEST_F(EngineSplit, ClassicalMeshReviewedIndependent) { classicalModel(true,0,true); parity(false); }
TEST_F(EngineSplit, ClassicalMeshReviewedEnslaved) { classicalModel(true,1,true); parity(false); }
TEST_F(EngineSplit, ClassicalMeshReviewedSigma) { classicalModel(true,2,true); parity(false); }
TEST_F(EngineSplit, ClassicalMeshReviewedGain) { classicalModel(true,0,true,true); parity(false); }
TEST_F(EngineSplit, ClassicalMeshOrdinarySources) { classicalModel(false,0,true); parity(false); }
TEST_F(EngineSplit, ClassicalMeshConduit) { classicalModel(false,0,true,false,2,false,true); parity(false); }
TEST_F(EngineSplit, ClassicalSealedReviewed) { classicalModel(true,0,true,false,1); parity(false); }
TEST_F(EngineSplit, ClassicalCurrentReviewed) { classicalModel(true,0,true,false,0); parity(false); }
TEST_F(EngineSplit, ClassicalMixedRichards) { classicalModel(true,0,true,false,2,true); parity(false); }
TEST_F(EngineSplit, ClassicalMixedRichardsSigma) { classicalModel(true,2,true,false,2,true); parity(false); }
TEST_F(EngineSplit, ClassicalEarlyFinish) { classicalModel(); parity(false,.7,false,false,true,1); }
TEST_F(EngineSplit, ReviewedMappedDischarge) { reviewedOutfallModel(true); parity(false); }
TEST_F(EngineSplit, ReviewedMappedWithdrawal) { reviewedOutfallModel(true,0,true,false,-1); parity(false); }
TEST_F(EngineSplit, ReviewedMappedFlapGate) { reviewedOutfallModel(true,0,false,false,0); parity(false); }
TEST_F(EngineSplit, ReviewedMappedRichards) { reviewedOutfallModel(true,0,true,true,-1); parity(false); }
TEST_F(EngineSplit, ReviewedReturnClosedShared) { reviewedOutfallModel(false); parity(false); }
TEST_F(EngineSplit, ReviewedReturnIndependent) { reviewedOutfallModel(false,0,true); parity(false); }
TEST_F(EngineSplit, ReviewedReturnEnslaved) { reviewedOutfallModel(false,1,true); parity(false); }
TEST_F(EngineSplit, ReviewedReturnSigma) { reviewedOutfallModel(false,2,true); parity(false); }
TEST_F(EngineSplit, ReviewedReturnRichards) { reviewedOutfallModel(false,0,true,true); parity(false); }
TEST_F(EngineSplit, ReviewedReturnEarlyFinish) { reviewedOutfallModel(false); parity(false,.7,false,false,true,1); }
TEST_F(EngineSplit, ReviewedReturnMixedMapped) { reviewedOutfallModel(false,0,true,false,0,false,true); mapped_direction=-1; parity(false); }
TEST_F(EngineSplit, ReviewedReturnOrdinary) { reviewedOutfallModel(false,0,true,false,0,true); parity(false); }
TEST_F(EngineSplit, ReviewedMappedReturnRefusesDuplicatePhysicalDestination) {
    reviewedOutfallModel(true);
    const std::string outfall="DRAIN 0 FREE NO";
    override_model.replace(override_model.find(outfall),outfall.size(),"DRAIN 0 FREE NO A");
    std::ofstream(model) << override_model;
    Engine surface; ASSERT_NO_FATAL_FAILURE(open(surface,"surface",false));
    ASSERT_EQ(swmm_split_configure(surface.value,SWMM_SPLIT_SURFACE),0);
    ASSERT_NO_FATAL_FAILURE(results(surface,"surface"));
    ASSERT_NO_FATAL_FAILURE(init(surface));
    SWMM_SplitFrame geometry=nullptr;
    EXPECT_NE(swmm_split_geometry(surface.value,&geometry),0);
    EXPECT_EQ(geometry,nullptr);
    EXPECT_FALSE(fs::exists(directory/"surface.h5"));
}
TEST_F(EngineSplit, UnknownOutfallReturnDestinationRefusesInsteadOfDiscardingWater) {
    reviewedOutfallModel(false);
    const std::string outfall="DRAIN 0 FREE NO A";
    override_model.replace(override_model.find(outfall),outfall.size(),"DRAIN 0 FREE NO UNKNOWN_CATCHMENT");
    std::ofstream(model) << override_model;
    Engine engine;
    EXPECT_NE(swmm_engine_open(engine.value,model.string().c_str(),(directory/"invalid.rpt").string().c_str(),nullptr,nullptr),0);
}
TEST_F(EngineSplit, JointRichardsClosedShared) { richardsModel(); parity(false); }
TEST_F(EngineSplit, JointRichardsClosedIndependent) { richardsModel(0,true); parity(false); }
TEST_F(EngineSplit, JointRichardsEnslavedShared) { richardsModel(1); parity(false); }
TEST_F(EngineSplit, JointRichardsEnslavedIndependent) { richardsModel(1,true); parity(false); }
TEST_F(EngineSplit, JointRichardsSigmaShared) { richardsModel(2); parity(false); }
TEST_F(EngineSplit, JointRichardsSigmaIndependent) { richardsModel(2,true); parity(false); }
TEST_F(EngineSplit, JointRichardsCompeting) { richardsModel(0,true,true); parity(false); }
TEST_F(EngineSplit, JointRichardsAquiferGain) {
    richardsModel(0,true); joint_aquifer_gain=true;
    const std::string from="SINK 0 .325 .024";
    override_model.replace(override_model.find(from),from.size(),"SINK -.2 .325 .024");
    const auto water=override_model.find("HG0 1");
    ASSERT_NE(water,std::string::npos); override_model.replace(water,5,"HG0 2");
    override_model += "\n[EVAPORATION]\nCONSTANT 48\n";
    parity(false);
}
TEST_F(EngineSplit, JointRichardsEarlyFinish) { richardsModel(); parity(false,.7,false,false,true,1); }
TEST_F(EngineSplit, JointRichardsTrialReplayStopsBeforeRouting) { richardsReplay(SWMM_SPLIT_LID_TRIAL); }
TEST_F(EngineSplit, JointRichardsAcceptanceReplayStopsBeforeRouting) { richardsReplay(SWMM_SPLIT_LID_ACCEPT); }
TEST_F(EngineSplit, ReviewedFootprintIndependent) { reviewedModel(0,true); parity(false); }
TEST_F(EngineSplit, ReviewedFootprintEnslaved) { reviewedModel(1,true); parity(false); }
TEST_F(EngineSplit, ReviewedFootprintSigma) { reviewedModel(2,true); parity(false); }
TEST_F(EngineSplit, ReviewedFootprintEvaporation) { reviewedModel(0,true,true); parity(false); }
TEST_F(EngineSplit, ReviewedFootprintEarlyFinish) { reviewedModel(); parity(false,.7,false,false,true,1); }
TEST_F(EngineSplit, ReviewedPreparationReplayStopsBeforeAdvancement) {
    reviewedModel(); std::ofstream(model) << override_model;
    Engine network,surface;
    ASSERT_NO_FATAL_FAILURE(open(network,"network"));
    ASSERT_NO_FATAL_FAILURE(open(surface,"surface",false));
    ASSERT_EQ(swmm_split_configure(network.value,SWMM_SPLIT_NETWORK),0);
    ASSERT_EQ(swmm_split_configure(surface.value,SWMM_SPLIT_SURFACE),0);
    ASSERT_NO_FATAL_FAILURE(results(surface,"surface"));
    ASSERT_NO_FATAL_FAILURE(init(surface));
    OwnedFrame geometry;
    ASSERT_EQ(swmm_split_geometry(surface.value,&geometry.value),0);
    ASSERT_NO_FATAL_FAILURE(init(network));
    ASSERT_EQ(swmm_split_apply_geometry(network.value,geometry.value),0);
    Peer peer; peer.surface=surface.value; peer.replay=true;
    ASSERT_EQ(swmm_split_set_callback(network.value,Peer::exchange,&peer),0);
    ASSERT_EQ(swmm_engine_start(surface.value,0),0);
    ASSERT_EQ(swmm_engine_start(network.value,1),0);
    double elapsed=0,time=-1;
    EXPECT_NE(swmm_engine_step(network.value,&elapsed),0);
    EXPECT_EQ(peer.source_preparations,1);
    ASSERT_EQ(swmm_engine_get_elapsed_seconds(surface.value,&time),0); EXPECT_EQ(time,0);
    ASSERT_EQ(swmm_engine_get_elapsed_seconds(network.value,&time),0); EXPECT_EQ(time,0);
}
TEST_F(EngineSplit, DetailedLidReportsRefuseBeforeInitialization) {
    watershedModel(true);
    const auto pos = extra_rows.find("0 * J");
    ASSERT_NE(pos,std::string::npos);
    extra_rows.replace(pos,5,"0 duplicate-lid.txt J");
    std::ofstream(model) << override_model << extra_rows;
    Engine network;
    ASSERT_NO_FATAL_FAILURE(open(network,"network"));
    EXPECT_NE(swmm_split_configure(network.value,SWMM_SPLIT_NETWORK),0);
    int state=-1;
    ASSERT_EQ(swmm_engine_get_state(network.value,&state),0);
    EXPECT_EQ(state,SWMM_STATE_OPENED);
    EXPECT_FALSE(fs::exists(directory / "duplicate-lid.txt"));
}
TEST_F(EngineSplit, MissingCallbackAndGeometryRefuseBeforeStart) {
    Engine network, surface;
    ASSERT_NO_FATAL_FAILURE(open(network, "network"));
    ASSERT_NO_FATAL_FAILURE(open(surface, "surface", false));
    ASSERT_EQ(swmm_split_configure(network.value, SWMM_SPLIT_NETWORK), SWMM_OK) << swmm_get_last_error_msg(network.value);
    ASSERT_EQ(swmm_split_configure(surface.value, SWMM_SPLIT_SURFACE), SWMM_OK);
    ASSERT_NO_FATAL_FAILURE(init(network));
    ASSERT_NO_FATAL_FAILURE(init(surface));
    EXPECT_EQ(swmm_engine_start(network.value, 1), SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(swmm_engine_start(surface.value, 0), SWMM_ERR_LIFECYCLE);
}
TEST_F(EngineSplit, ManagedCheckpointCannotEnrollASplitParticipant) {
    Engine network;
    ASSERT_NO_FATAL_FAILURE(open(network, "network"));
    ASSERT_EQ(swmm_split_configure(network.value, SWMM_SPLIT_NETWORK), SWMM_OK);
    SWMM_CheckpointSessionOptions options{};
    options.struct_size = sizeof(options); options.schema_version = 1;
    options.output = SWMM_CHECKPOINT_OUTPUT_NONE;
    EXPECT_NE(swmm_checkpoint_start_session(network.value, &options), SWMM_OK);
    int state = -1;
    ASSERT_EQ(swmm_engine_get_state(network.value, &state), SWMM_OK);
    EXPECT_EQ(state, SWMM_STATE_OPENED);
}
TEST_F(EngineSplit, ReplayedRequestStopsBothParticipants) {
    Engine network, surface; Peer peer;
    ASSERT_NO_FATAL_FAILURE(open(network, "network"));
    ASSERT_NO_FATAL_FAILURE(open(surface, "surface", false));
    ASSERT_NO_FATAL_FAILURE(connect(network, surface, peer));
    peer.replay = true;
    ASSERT_EQ(swmm_engine_start(surface.value, 0), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(network.value, 1), SWMM_OK);
    double elapsed = 0;
    EXPECT_NE(swmm_engine_step(network.value, &elapsed), SWMM_OK);
    EXPECT_NE(swmm_engine_step(network.value, &elapsed), SWMM_OK);
}
TEST_F(EngineSplit, OutfallPreparationCannotBeReplayed) {
    outfallModel(OutfallDirection::Withdraw);
    std::ofstream(model) << override_model;
    Engine network, surface;
    Peer peer;
    ASSERT_NO_FATAL_FAILURE(open(network,"network"));
    ASSERT_NO_FATAL_FAILURE(open(surface,"surface",false));
    ASSERT_NO_FATAL_FAILURE(connect(network,surface,peer));
    peer.replay = true;
    ASSERT_EQ(swmm_engine_start(surface.value,0),SWMM_OK);
    ASSERT_EQ(swmm_engine_start(network.value,1),SWMM_OK);
    double elapsed = 0, time = -1;
    EXPECT_NE(swmm_engine_step(network.value,&elapsed),SWMM_OK);
    ASSERT_EQ(swmm_engine_get_elapsed_seconds(network.value,&time),SWMM_OK);
    EXPECT_EQ(time,0);
    ASSERT_EQ(swmm_engine_get_elapsed_seconds(surface.value,&time),SWMM_OK);
    EXPECT_EQ(time,0);
    EXPECT_EQ(peer.preparations,1);
}
TEST_F(EngineSplit, BorrowedRequestCannotBeReturnedAsOwnedReply) {
    Engine network, surface; Peer peer;
    ASSERT_NO_FATAL_FAILURE(open(network, "network"));
    ASSERT_NO_FATAL_FAILURE(open(surface, "surface", false));
    ASSERT_NO_FATAL_FAILURE(connect(network, surface, peer));
    peer.alias = true;
    ASSERT_EQ(swmm_engine_start(surface.value, 0), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(network.value, 1), SWMM_OK);
    double elapsed = 0;
    EXPECT_NE(swmm_engine_step(network.value, &elapsed), SWMM_OK);
}
TEST_F(EngineSplit, CallbackExceptionStopsWithoutUnwindingAcrossHydraulicTeam) {
    Engine network, surface; Peer peer;
    ASSERT_NO_FATAL_FAILURE(open(network, "network"));
    ASSERT_NO_FATAL_FAILURE(open(surface, "surface", false));
    ASSERT_NO_FATAL_FAILURE(connect(network, surface, peer));
    peer.throws = true;
    ASSERT_EQ(swmm_engine_start(surface.value, 0), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(network.value, 1), SWMM_OK);
    double elapsed = 0, before = 0, after = 0;
    swmm_get_current_time(network.value, &before);
    EXPECT_NE(swmm_engine_step(network.value, &elapsed), SWMM_OK);
    const std::string error = swmm_get_last_error_msg(network.value);
    EXPECT_NE(error.find("callback threw"), std::string::npos);
    EXPECT_NE(swmm_engine_step(network.value, &elapsed), SWMM_OK);
    EXPECT_EQ(swmm_get_last_error_msg(network.value), error);
    swmm_get_current_time(network.value, &after); EXPECT_EQ(before, after);
}
TEST_F(EngineSplit, WrongReceivingInstanceIsRefusedBeforeAdvancement) {
    Engine network, surface, stranger; Peer peer;
    ASSERT_NO_FATAL_FAILURE(open(network, "network"));
    ASSERT_NO_FATAL_FAILURE(open(surface, "surface", false));
    ASSERT_NO_FATAL_FAILURE(connect(network, surface, peer));
    ASSERT_NO_FATAL_FAILURE(open(stranger, "stranger", false));
    ASSERT_EQ(swmm_split_configure(stranger.value, SWMM_SPLIT_SURFACE), SWMM_OK);
    ASSERT_NO_FATAL_FAILURE(init(stranger)); OwnedFrame g;
    ASSERT_EQ(swmm_split_geometry(stranger.value, &g.value), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(stranger.value, 0), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(surface.value, 0), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(network.value, 1), SWMM_OK);
    peer.surface = stranger.value;
    double before = 0, after = 0, elapsed = 0;
    swmm_get_current_time(stranger.value, &before);
    EXPECT_NE(swmm_engine_step(network.value, &elapsed), SWMM_OK);
    swmm_get_current_time(stranger.value, &after); EXPECT_EQ(before, after);
}
TEST_F(EngineSplit, TransportAndAuthoredWriterAreRefusedAtConfiguration) {
    for (const auto* option : {"TRANSPORT_AGE YES", "OUTPUT_FILE authored.h5", "RECONSTRUCTION_ORDER 2"}) {
        writeModel(false);
        std::ofstream(model, std::ios::app) << "\n[2D_OPTIONS]\n" << option << '\n';
        Engine surface;
        ASSERT_NO_FATAL_FAILURE(open(surface, "surface", false));
        EXPECT_NE(swmm_split_configure(surface.value, SWMM_SPLIT_SURFACE), SWMM_OK) << option;
    }
    EXPECT_FALSE(fs::exists(directory / "authored.h5"));
}
TEST_F(EngineSplit, DisabledConfiguredAquiferIsRefusedBeforeInitialization) {
    std::ofstream(model, std::ios::app) << R"(
[2D_AQUIFER_OPTIONS]
MODE MESH
SOIL_CHAR GARDNER
CLOSURE CLOSED_FORM
[2D_AQUIFER]
* 36 8 .45 .1 2 HG0 2
[2D_OPTIONS]
GROUNDWATER NO
)";
    Engine surface;
    ASSERT_NO_FATAL_FAILURE(open(surface, "surface", false));
    EXPECT_NE(swmm_split_configure(surface.value, SWMM_SPLIT_SURFACE), SWMM_OK);
    int state = -1; swmm_engine_get_state(surface.value, &state); EXPECT_EQ(state, SWMM_STATE_OPENED);
}
TEST_F(EngineSplit, ParallelHydraulicCallbacksAreRefusedBeforeInitialization) {
    std::ofstream(model, std::ios::app) << "\n[OPTIONS]\nTHREADS 2\n";
    Engine network;
    ASSERT_NO_FATAL_FAILURE(open(network, "network"));
    EXPECT_NE(swmm_split_configure(network.value, SWMM_SPLIT_NETWORK), SWMM_OK);
    int state = -1; swmm_engine_get_state(network.value, &state); EXPECT_EQ(state, SWMM_STATE_OPENED);
}
TEST_F(EngineSplit, MappedOutfallIsResolvedBeforeStart) {
    std::ofstream(model, std::ios::app) << "\n[2D_TRIANGLE_NODE_MAP]\n1 O .6 .02\n";
    Engine surface;
    ASSERT_NO_FATAL_FAILURE(open(surface, "surface", false));
    ASSERT_EQ(swmm_split_configure(surface.value, SWMM_SPLIT_SURFACE), SWMM_OK);
    ASSERT_NO_FATAL_FAILURE(init(surface));
    OwnedFrame geometry;
    EXPECT_EQ(swmm_split_geometry(surface.value, &geometry.value), SWMM_OK);
    EXPECT_NE(geometry.value, nullptr);
}
TEST_F(EngineSplit, MismatchedOriginalConfigurationsCannotConnect) {
    Engine network, surface;
    ASSERT_NO_FATAL_FAILURE(open(network, "network"));
    ASSERT_EQ(swmm_split_configure(network.value, SWMM_SPLIT_NETWORK), SWMM_OK);
    writeModel(true);
    ASSERT_NO_FATAL_FAILURE(open(surface, "surface", false));
    ASSERT_EQ(swmm_split_configure(surface.value, SWMM_SPLIT_SURFACE), SWMM_OK);
    ASSERT_NO_FATAL_FAILURE(init(network));
    ASSERT_NO_FATAL_FAILURE(init(surface));
    OwnedFrame geometry;
    ASSERT_EQ(swmm_split_geometry(surface.value, &geometry.value), SWMM_OK);
    EXPECT_NE(swmm_split_apply_geometry(network.value, geometry.value), SWMM_OK);
}
}
