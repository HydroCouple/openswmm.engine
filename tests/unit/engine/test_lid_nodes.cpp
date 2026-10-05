// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "core/SWMMEngine.hpp"
#include "core/InpWriter.hpp"
#include "plugins/DefaultInputPlugin.hpp"
#include "input/PostParseResolver.hpp"
#include "hydrology/LidNode.hpp"
#include "quality/QualityRouting.hpp"
#include "hydraulics/Node.hpp"
#include "openswmm/engine/openswmm_infrastructure.h"
#include "openswmm/engine/openswmm_links.h"
#include "openswmm/engine/openswmm_pollutants.h"
#include "core/HotStartManager.hpp"
#include "input/geopackage/GeoPackageReader.hpp"
#include "input/geopackage/GeoPackageWriter.hpp"
#include "edit/ObjectDeleter.hpp"
#include "edit/TypeConverter.hpp"
#include <filesystem>
#include <fstream>
#include <cmath>
#include <limits>
#include <numeric>
using namespace openswmm;
namespace {
SimulationContext model() {
    SimulationContext c;
    c.nodes.resize(2);
    c.node_names.add("S"); c.node_names.add("O");
    c.node_subtypes.set_node_type(c.nodes, 0, NodeType::STORAGE);
    c.node_subtypes.set_node_type(c.nodes, 1, NodeType::OUTFALL);
    c.node_subtypes.storages.c[0] = 100.0;
    c.nodes.full_depth[0] = 2.0;
    c.lid_names.add("Stack");
    c.lid_controls.names = {"Stack"}; c.lid_controls.lid_type = {"NODE"};
    c.lid_controls.node_layers = {{
        {LidNodeLayerKind::Surface, {6, 0.1}},
        {LidNodeLayerKind::Media, {12, .45, .2, .08, 2, 10, 3}},
        {LidNodeLayerKind::Aggregate, {6, .4, 100}},
        {LidNodeLayerKind::Bottom, {.5, 0}}}};
    c.node_subtypes.storages.lid[0] = {0, 10};
    return c;
}
}
TEST(LidNodes, ArbitraryOrderedLayersAndValidation) {
    auto c = model();
    auto& stack = c.lid_controls.node_layers[0];
    for (int i = 0; i < 40; ++i)
        stack.insert(stack.end() - 1, {LidNodeLayerKind::Media, {1, .5, .2, .1, 2, 5, 3}});
    EXPECT_EQ(stack.size(), 44u);
    EXPECT_TRUE(lidnode::validateStack(stack).empty());
    EXPECT_DOUBLE_EQ(lidnode::thickness(stack), 64.0);
    lidnode::sync(c, 0);
    EXPECT_NEAR(c.nodes.full_depth[0], 64.0 / 12.0, 1.e-14);
    lidnode::validate(c);
    EXPECT_TRUE(c.errors.empty());
    stack[2].params[1] = 1.1;
    EXPECT_FALSE(lidnode::validateStack(stack).empty());
}
TEST(LidNodes, RejectsDepthMismatchAndDuplicateAssignment) {
    auto c = model();
    c.nodes.full_depth[0] = 4;
    lidnode::validate(c);
    ASSERT_EQ(c.errors.size(), 1u);
    EXPECT_NE(c.errors[0].find("MaxDepth"), std::string::npos);
    lidnode::readNodes(c, {"S Stack 10"});
    EXPECT_EQ(c.errors.size(), 2u);
}
TEST(LidNodes, InputRoundTripPreservesRepeatedLayers) {
    auto c = model();
    auto& stack = c.lid_controls.node_layers[0];
    stack.insert(stack.begin() + 2, {LidNodeLayerKind::Media, {12, .4, .25, .1, .75, 8, 5}});
    lidnode::sync(c, 0);
    auto dir = std::filesystem::path(OPENSWMM_TEST_SOURCE_DIR) / "../../output/lid_nodes_2026_10_04";
    std::filesystem::create_directories(dir);
    const auto path = (dir / "roundtrip.inp").string();
    ASSERT_EQ(inp_writer::writeInpFile(c, path), 0);
    SimulationContext reread;
    DefaultInputPlugin plugin;
    ASSERT_EQ(plugin.read(path, reread), 0);
    input::resolve_cross_references(reread);
    ASSERT_TRUE(reread.errors.empty()) << (reread.errors.empty() ? "" : reread.errors.front());
    ASSERT_EQ(reread.lid_controls.node_layers[0].size(), 5u);
    EXPECT_DOUBLE_EQ(reread.lid_controls.node_layers[0][2].params[4], .75);
    EXPECT_EQ(reread.node_subtypes.storages.lid[0].control, 0);
    EXPECT_DOUBLE_EQ(reread.node_subtypes.storages.lid[0].initial_saturation, 10);
}
TEST(LidNodes, AtomicApiStackAndStorageAssignment) {
    SWMMEngine engine;
    engine.context() = model();
    engine.context().state = EngineState::OPENED;
    auto h = reinterpret_cast<SWMM_Engine>(&engine);
    SWMM_LidNodeLayer rows[] = {{2, {24, .4, 30}}};
    ASSERT_EQ(swmm_lid_node_layers_set(h, 0, rows, 1), 0);
    EXPECT_EQ(swmm_lid_node_layer_count(h, 0), 1);
    EXPECT_DOUBLE_EQ(engine.context().nodes.full_depth[0], 2);
    rows[0].params[1] = 1.2;
    EXPECT_NE(swmm_lid_node_layers_set(h, 0, rows, 1), 0);
    SWMM_LidNodeLayer out{};
    ASSERT_EQ(swmm_lid_node_layer_get(h, 0, 0, &out), 0);
    EXPECT_DOUBLE_EQ(out.params[1], .4);
    EXPECT_NE(swmm_node_set_lid(h, 1, 0, 10), 0);
    EXPECT_EQ(swmm_node_set_lid(h, 0, -1, 0), 0);
    int control; double sat;
    EXPECT_EQ(swmm_node_get_lid(h, 0, &control, &sat), 0);
    EXPECT_EQ(control, -1);
}
TEST(LidNodes, RetainedAndMobileStorageConserveWater) {
    auto c = model();
    lidnode::initialize(c);
    const double initial = lidnode::heldVolume(c, 0);
    EXPECT_GT(initial, 0.0);
    const double totalCapacity = node::getVolume(c.nodes, 0, 2, &c.tables, 0, &c.node_subtypes) + initial;
    EXPECT_NEAR(totalCapacity, 100 * (.5 * .9 + 1 * .45 + .5 * .4), 1.e-10);
    for (int step = 0; step < 100; ++step) {
        c.nodes.lat_flow[0] = .01;
        lidnode::prepareStep(c, 10.0, 0.0);
        // Closed-bucket hydraulic balance of the uncaptured inflow.
        c.nodes.volume[0] += c.nodes.lat_flow[0] * 10;
        lidnode::finishStep(c);
        EXPECT_NEAR(c.nodes.volume[0] + lidnode::heldVolume(c, 0), initial + .1 * (step + 1), 1.e-9);
        for (const auto& cell : c.node_subtypes.storages.lid_state[0].cells) {
            EXPECT_GE(cell.theta, cell.wilting_point - 1.e-14);
            EXPECT_LE(cell.theta, cell.porosity + 1.e-14);
        }
    }
}
TEST(LidNodes, MobileDepthVolumeInverseAcrossEveryLayer) {
    auto c = model(); lidnode::initialize(c);
    for (double depth : {.1, .49, .5, .55, 1.2, 1.49, 1.5, 1.8, 2.0}) {
        double v = node::getVolume(c.nodes, 0, depth, &c.tables, 0, &c.node_subtypes);
        EXPECT_NEAR(node::getDepth(c.nodes, 0, v, &c.tables, 0, &c.node_subtypes), depth, 1.e-12);
    }
}
TEST(LidNodes, RoutedBucketConservation) {
    auto dir = std::filesystem::path(OPENSWMM_TEST_SOURCE_DIR) / "../../output/lid_nodes_2026_10_04";
    std::filesystem::create_directories(dir);
    const auto inp = (dir / "bucket.inp").string();
    std::ofstream f(inp);
    f << R"([OPTIONS]
FLOW_UNITS CFS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2004
END_DATE 01/01/2004
END_TIME 00:10:00
ROUTING_STEP 00:00:01
VARIABLE_STEP 0
REPORT_STEP 00:01:00
IGNORE_QUALITY YES
[STORAGE]
S 0 2 0 FUNCTIONAL 0 0 100 0 0
[OUTFALLS]
O 0 FREE NO
[ORIFICES]
D S O BOTTOM 0 0.6 NO 0
[XSECTIONS]
D CIRCULAR .1 0 0 0
[DWF]
S FLOW .01
[LID_CONTROLS]
Stack NODE
Stack SURFACE 6 .1
Stack MEDIA 12 .45 .2 .08 2 10 3
Stack AGGREGATE 6 .4 100
[LID_NODES]
S Stack 10
[LID_NODE_OUTLETS]
D 3 BOTTOM
)";
    f.close();
    SWMMEngine e;
    ASSERT_EQ(e.open(inp.c_str(), (dir / "bucket.rpt").string().c_str(), (dir / "bucket.out").string().c_str()), 0);
    ASSERT_EQ(e.initialize(), 0);
    ASSERT_EQ(e.start(1), 0);
    double t = 0;
    do { ASSERT_EQ(e.step(&t), 0); } while (t > 0);
    ASSERT_EQ(e.end(), 0);
    auto& mb = e.context().mass_balance;
    const double initial = mb.routing_init_storage;
    const double final = mb.routing_final_storage;
    const double input = mb.routing_dry_weather + mb.routing_external;
    const double output = mb.routing_outflow + mb.routing_flooding + mb.routing_evap_loss + mb.routing_seep_loss;
    EXPECT_NEAR(initial + input, final + output, 0.02) << "initial=" << initial << " final=" << final << " input=" << input << " output=" << output;
    e.close();
}

TEST(LidNodes, HotstartRetainsMoistureAndCloggingHistory) {
    auto c = model(); c.pollutant_names.add("TSS"); c.pollutants.resize_pollutants(1); lidnode::initialize(c);
    c.node_subtypes.storages.lid_state[0].quality_mass.assign(c.node_subtypes.storages.lid_state[0].cells.size(), 12.5);
    c.nodes.lat_flow[0] = .1;
    lidnode::prepareStep(c, 20, 0); lidnode::finishStep(c);
    const auto saved = c.node_subtypes.storages.lid_state[0];
    const auto path = (std::filesystem::path(OPENSWMM_TEST_SOURCE_DIR) / "../../output/lid_nodes_2026_10_04/profile.hsf").string();
    std::unique_ptr<HotStartFile> hs(HotStartManager::save(c, path));
    ASSERT_TRUE(hs); EXPECT_EQ(hs->header.version, 9u);
    hs.reset(HotStartManager::open(path)); ASSERT_TRUE(hs);
    lidnode::initialize(c);
    ASSERT_EQ(HotStartManager::apply(*hs, c), 0);
    const auto& restored = c.node_subtypes.storages.lid_state[0];
    EXPECT_DOUBLE_EQ(restored.held_volume, saved.held_volume);
    EXPECT_DOUBLE_EQ(restored.treated_volume, saved.treated_volume);
    EXPECT_EQ(restored.quality_mass, saved.quality_mass);
    for (std::size_t k = 0; k < saved.cells.size(); ++k)
        EXPECT_DOUBLE_EQ(restored.cells[k].theta, saved.cells[k].theta);
    c.node_subtypes.storages.lid_state[0].cells.pop_back();
    EXPECT_GT(HotStartManager::apply(*hs, c), 0);
}
#ifdef LID_TEST_GEOPACKAGE
TEST(LidNodes, GeoPackagePreservesOrderedLayersAndAssignment) {
    auto c = model();
    c.pollutant_names.add("TSS"); c.pollutants.resize_pollutants(1);
    c.lid_controls.node_layers[0][1].treatment={{"TSS",.25,1.5,"R = 0.2"}};
    // Populate all standard parallel columns as a parsed model would.
    c.lid_controls.surface.resize(1); c.lid_controls.soil.resize(1);
    c.lid_controls.storage.resize(1); c.lid_controls.pavement.resize(1);
    c.lid_controls.drain.resize(1); c.lid_controls.drainmat.resize(1); c.lid_controls.removals.resize(1);
    const auto path = (std::filesystem::path(OPENSWMM_TEST_SOURCE_DIR) / "../../output/lid_nodes_2026_10_04/profile.gpkg").string();
    ASSERT_EQ(gpkg::write_to_file(path, c, "lid"), 0);
    SimulationContext restored;
    ASSERT_EQ(gpkg::read_from_file(path, restored, "lid"), 0);
    ASSERT_EQ(restored.lid_controls.node_layers.size(), 1u);
    ASSERT_EQ(restored.lid_controls.node_layers[0].size(), 4u);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(restored.lid_controls.node_layers[0][i].kind, c.lid_controls.node_layers[0][i].kind);
        EXPECT_EQ(restored.lid_controls.node_layers[0][i].params, c.lid_controls.node_layers[0][i].params);
    }
    EXPECT_DOUBLE_EQ(restored.node_subtypes.storages.lid[0].initial_saturation, 10);
    ASSERT_EQ(restored.lid_controls.node_layers[0][1].treatment.size(), 1u);
    const auto& rule=restored.lid_controls.node_layers[0][1].treatment[0];
    EXPECT_EQ(rule.pollutant,"TSS"); EXPECT_DOUBLE_EQ(rule.removal,.25);
    EXPECT_DOUBLE_EQ(rule.decay,1.5); EXPECT_EQ(rule.expression,"R = 0.2");
}
#endif
TEST(LidNodes, OrdinaryLinksAreNumericallyUnchanged) {
    auto c = model();
    c.links.resize(1); c.links.node1[0] = 0; c.links.node2[0] = 1;
    const double q = .123456789123456789;
    EXPECT_DOUBLE_EQ(lidnode::exchangePorts(c, 0, q, .783943), q);
}
TEST(LidNodes, PortsRespectAvailableWaterAndBackflowCapacity) {
    auto c = model(); lidnode::initialize(c);
    c.links.resize(1); c.links.node1[0] = 0; c.links.node2[0] = 1;
    c.links.offset1[0] = 1.5;
    c.node_subtypes.storages.lid_state[0].cells[0].theta = .1;
    lidnode::resetPorts(c);
    const double q = lidnode::exchangePorts(c, 0, 100, 10);
    EXPECT_NEAR(q, .5, 1.e-12);
    EXPECT_DOUBLE_EQ(lidnode::exchangePorts(c, 0, 100, 10), 0);
    lidnode::resetPorts(c);
    EXPECT_DOUBLE_EQ(lidnode::exchangePorts(c, 0, -100, 10), -100);
    EXPECT_NEAR(c.node_subtypes.storages.lid_state[0].port_delta[0], 40, 1.e-12);
}

TEST(LidNodes, ElevatedPortsConserveAcrossPondingAndSurcharge) {
    const auto dir = std::filesystem::path(OPENSWMM_TEST_SOURCE_DIR) / "../../output/lid_nodes_2026_10_04";
    for (const bool reverse : {false, true}) for (const bool conduit : {false, true}) {
        const auto stem = (dir / (std::string(reverse ? "backflow" : "overflow") + (conduit ? "_pipe" : ""))).string();
        std::ofstream f(stem + ".inp");
        f << "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2004\nEND_DATE 01/01/2004\nEND_TIME 00:05:00\nROUTING_STEP 00:00:01\nVARIABLE_STEP 0\nREPORT_STEP 00:01:00\nIGNORE_QUALITY YES\n"
          << "[STORAGE]\nS 0 2 0 FUNCTIONAL 0 0 100 0 0\n[OUTFALLS]\nO 0 "
          << (reverse ? "FIXED 2.5" : "FREE") << " NO\n" << (conduit ? "[CONDUITS]\nD S O 10 .013 1.5 0 0 0\n" : "[ORIFICES]\nD S O SIDE 1.5 0.6 NO 0\n") << "[XSECTIONS]\nD CIRCULAR .2 0 0 0\n[DWF]\nS FLOW "
          << (reverse ? 0.0 : 1.0)
          << "\n[LID_CONTROLS]\nStack NODE\nStack SURFACE 6 .1\nStack MEDIA 12 .45 .2 .08 .01 10 3\nStack AGGREGATE 6 .4 100\n[LID_NODES]\nS Stack 10\n";
        f.close();
        SWMMEngine e;
        ASSERT_EQ(e.open((stem+".inp").c_str(), (stem+".rpt").c_str(), (stem+".out").c_str()), 0);
        ASSERT_EQ(e.initialize(), 0); ASSERT_EQ(e.start(1), 0);
        double t = 0;
        do { ASSERT_EQ(e.step(&t), 0); } while (t > 0);
        ASSERT_EQ(e.end(), 0);
        const auto& mb = e.context().mass_balance;
        EXPECT_NEAR(mb.routing_init_storage + mb.routing_dry_weather + mb.routing_external,
                    mb.routing_final_storage + mb.routing_outflow + mb.routing_flooding + mb.routing_evap_loss + mb.routing_seep_loss, .1) << stem;
        for (const auto& c : e.context().node_subtypes.storages.lid_state[0].cells) {
            EXPECT_LE(c.theta, c.porosity + 1.e-12); EXPECT_GE(c.theta, c.wilting_point - 1.e-12);
        }
        e.close();
    }
}

TEST(LidNodes, FullySaturatedInitialProfileHasConnectedWaterTable) {
    auto c = model(); c.node_subtypes.storages.lid[0].initial_saturation = 100;
    lidnode::initialize(c);
    EXPECT_NEAR(lidnode::heldVolume(c, 0) + c.nodes.volume[0], 65, 1.e-12);
    EXPECT_GT(c.nodes.volume[0], 0);
    EXPECT_NEAR(node::getDepth(c.nodes, 0, c.nodes.volume[0], &c.tables, 0, &c.node_subtypes), 1.5, 1.e-12);
}

TEST(LidNodes, ConvertingStorageClearsOutletAnchors) {
    auto c = model(); c.links.resize(1); c.links.node1[0] = 0; c.links.node2[0] = 1;
    c.lid_node_outlets.push_back({0, 3, false});
    edit::convert_node(c, 0, NodeType::JUNCTION);
    EXPECT_TRUE(c.lid_node_outlets.empty());
    EXPECT_EQ(c.node_subtypes.storage_row(0), -1);
}

TEST(LidNodes, UndoingAddedOutletClearsItsAnchor) {
    SWMMEngine engine;
    engine.context() = model(); engine.context().state = EngineState::OPENED;
    const auto h = reinterpret_cast<SWMM_Engine>(&engine);
    ASSERT_EQ(swmm_link_add(h, "Drain", SWMM_LINK_ORIFICE), 0);
    ASSERT_EQ(swmm_link_set_nodes(h, 0, 0, 1), 0);
    ASSERT_EQ(swmm_lid_node_outlet_set(h, 0, 3, 0), 0);
    ASSERT_EQ(swmm_link_pop_last(h, "Drain"), 0);
    EXPECT_TRUE(engine.context().lid_node_outlets.empty());
}

TEST(LidNodes, SurchargeWetsMediaToFieldCapacityConservatively) {
    auto c = model(); lidnode::initialize(c);
    c.nodes.depth[0] = 1.5;
    c.nodes.volume[0] = node::getVolume(c.nodes, 0, 1.5, &c.tables, 0, &c.node_subtypes);
    const double before = c.nodes.volume[0] + lidnode::heldVolume(c, 0);
    lidnode::prepareStep(c, 1, 0);
    EXPECT_NEAR(c.nodes.volume[0] + lidnode::heldVolume(c, 0), before, 1.e-12);
    for (const auto& cell : c.node_subtypes.storages.lid_state[0].cells)
        if (cell.kind == LidNodeLayerKind::Media) EXPECT_DOUBLE_EQ(cell.theta, cell.field_capacity);
}

TEST(LidNodes, LayerTreatmentApiIsAtomicAndPreservedByPhysicalEdits) {
    SWMMEngine e;e.context()=model();auto& c=e.context();c.state=EngineState::OPENED;
    c.pollutant_names.add("TSS");c.pollutants.resize_pollutants(1);
    const auto h=reinterpret_cast<SWMM_Engine>(&e);
    SWMM_LidNodeLayer rows[]={{1,{12,.45,.2,.08,2,10,3}},{2,{12,.4,100}}};
    SWMM_LidLayerTreatment rule{1,0,25,1.5,"R = 0.2"};
    ASSERT_EQ(swmm_lid_node_configure(h,0,rows,2,&rule,1),SWMM_OK);
    ASSERT_EQ(swmm_lid_node_treatment_count(h,0),1);
    rows[0].params[0]=18;
    ASSERT_EQ(swmm_lid_node_layers_set(h,0,rows,2),SWMM_OK);
    SWMM_LidLayerTreatment got{};ASSERT_EQ(swmm_lid_node_treatment_get(h,0,0,&got),SWMM_OK);
    EXPECT_DOUBLE_EQ(got.decay_per_day,1.5);EXPECT_STREQ(got.expression,"R = 0.2");
    rule.removal_percent=101;
    EXPECT_NE(swmm_lid_node_configure(h,0,rows,2,&rule,1),SWMM_OK);
    ASSERT_EQ(swmm_lid_node_treatment_get(h,0,0,&got),SWMM_OK);EXPECT_DOUBLE_EQ(got.removal_percent,25);
    rule.removal_percent=25;rule.expression="R = nonsense(";
    EXPECT_NE(swmm_lid_node_configure(h,0,rows,2,&rule,1),SWMM_OK);
}
TEST(LidNodes, LayerRatesExpressionsAndTransfersConservePollutantMass) {
    auto c=model();c.pollutant_names.add("TSS");c.pollutants.resize_pollutants(1);
    c.nodes.conc_old.assign(2,10);c.nodes.conc.assign(2,10);c.nodes.qual_mass_in.assign(2,0);c.nodes.qual_vol_in.assign(2,0);
    c.mass_balance.qual_routing_reacted.assign(1,0);
    lidnode::initialize(c);auto& state=c.node_subtypes.storages.lid_state[0];
    for(auto& cell:state.cells)cell.theta=.1;
    state.quality_old_water.clear();for(const auto& cell:state.cells)state.quality_old_water.push_back(cell.theta*cell.geometric_volume);
    state.quality_old_mobile=0;c.nodes.old_volume[0]=0;
    // One transfer across a layer boundary; fixed removal and expression compose.
    state.quality_transfers={{0,1,1}};
    c.lid_controls.node_layers[0][0].treatment={{"TSS",.25,0,"R = 0.2"}};
    c.lid_controls.node_layers[0][1].treatment={{"TSS",0,86400*std::log(2.0),""}};
    const double before=10*std::accumulate(state.quality_old_water.begin(),state.quality_old_water.end(),0.0);
    lidnode::prepareQuality(c,1);
    const double after=std::accumulate(state.quality_mass.begin(),state.quality_mass.end(),0.0);
    EXPECT_NEAR(before,after+c.mass_balance.qual_routing_reacted[0],1.e-10);
    // First media cell starts at 20 mass, receives 6, then halves in one second.
    EXPECT_NEAR(state.quality_mass[1],13,1.e-10);
    EXPECT_NEAR(state.quality_mass[0],40,1.e-10);
}
TEST(LidNodes, LayerTreatmentInputRoundTrip) {
    auto c=model();c.pollutant_names.add("TSS");c.pollutants.resize_pollutants(1);
    c.lid_controls.node_layers[0][1].treatment={{"TSS",.25,1.5,"R = 0.2"}};
    const auto path=(std::filesystem::path(OPENSWMM_TEST_SOURCE_DIR)/"../../output/lid_nodes_2026_10_04/treatment_roundtrip.inp").string();
    ASSERT_EQ(inp_writer::writeInpFile(c,path),0);
    SimulationContext read;DefaultInputPlugin plugin;ASSERT_EQ(plugin.read(path,read),0);input::resolve_cross_references(read);
    ASSERT_TRUE(read.errors.empty())<<(read.errors.empty()?"":read.errors[0]);
    const auto& rules=read.lid_controls.node_layers[0][1].treatment;ASSERT_EQ(rules.size(),1u);
    EXPECT_DOUBLE_EQ(rules[0].removal,.25);EXPECT_EQ(rules[0].expression,"R = 0.2");
}

TEST(LidNodes, RoutedLayerTreatmentMassBalance) {
    const auto dir = std::filesystem::path(OPENSWMM_TEST_SOURCE_DIR) / "../../output/lid_nodes_2026_10_04";
    for (double inflow : {.01, 1.0}) for (bool treatment : {false, true}) {
        const auto stem = "quality_" + std::to_string(inflow) + (treatment ? "_treated" : "_plain");
        const auto path = (dir / (stem + ".inp")).string();
        std::ofstream f(path);
        f << R"([OPTIONS]
FLOW_UNITS CFS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2004
END_DATE 01/01/2004
END_TIME 00:05:00
ROUTING_STEP 0.1
VARIABLE_STEP 0
REPORT_STEP 00:01:00
[STORAGE]
S 0 2 0 FUNCTIONAL 0 0 100 0 0
[OUTFALLS]
O 0 FREE NO
[ORIFICES]
D S O SIDE 1.5 .6 NO 0
[XSECTIONS]
D CIRCULAR .2 0 0 0
[POLLUTANTS]
TSS MG/L 0 0 0 0 NO * 0 0 0
[DWF]
S TSS 10
S FLOW )" << inflow << R"(
[LID_CONTROLS]
Stack NODE
Stack SURFACE 6 .1
Stack MEDIA 12 .45 .2 .08 2 10 3
Stack AGGREGATE 6 .4 100
[LID_NODES]
S Stack 10
[LID_NODE_OUTLETS]
D 1 BOTTOM
)";
        if (treatment) f << "[LID_LAYER_TREATMENT]\nStack 1 TSS 25 1 R = 0.2\nStack 2 TSS 10 2 C = C * 0.9\n";
        f.close();
        SWMMEngine e;
        ASSERT_EQ(e.open(path.c_str(), (dir/(stem+".rpt")).string().c_str(), (dir/(stem+".out")).string().c_str()),0);
        ASSERT_EQ(e.initialize(),0); ASSERT_EQ(e.start(1),0);
        double t=0; do { ASSERT_EQ(e.step(&t),0); } while(t>0);
        ASSERT_EQ(e.end(),0);
        const auto& b=e.context().mass_balance;
        const double input=b.qual_routing_init[0]+b.qual_routing_dw_in[0]+b.qual_routing_ex_in[0];
        const double output=b.qual_routing_final[0]+b.qual_routing_outflow[0]+b.qual_routing_flood[0]+b.qual_routing_reacted[0]+b.qual_routing_seep[0]+b.qual_routing_final_dry[0];
        EXPECT_NEAR(input,output,std::max(.001,input*.005)) << stem << " stored=" << b.qual_routing_final[0] << " out=" << b.qual_routing_outflow[0] << " reacted=" << b.qual_routing_reacted[0];
        if(treatment) EXPECT_GT(b.qual_routing_reacted[0],0);
        e.close();
    }
}

TEST(LidNodes, SaturatedAggregateDecayConservesMass) {
    auto c=model(); c.pollutant_names.add("TSS"); c.pollutants.resize_pollutants(1);
    c.lid_controls.node_layers[0]={{LidNodeLayerKind::Aggregate,{24,.4,100},{{"TSS",0,86400*std::log(2.0),""}}}};
    c.node_subtypes.storages.lid[0].initial_saturation=100;
    c.nodes.conc_old.assign(2,10); c.nodes.conc.assign(2,10);
    c.nodes.qual_mass_in.assign(2,0); c.nodes.qual_vol_in.assign(2,0);
    c.mass_balance.qual_routing_reacted.assign(1,0);
    lidnode::initialize(c);
    auto& state=c.node_subtypes.storages.lid_state[0];
    state.quality_old_water.assign(state.cells.size(),0);
    state.quality_old_mobile=80; c.nodes.old_volume[0]=80; c.nodes.old_depth[0]=2;
    lidnode::prepareQuality(c,1);
    EXPECT_NEAR(c.nodes.conc_old[0],5,1.e-10);
    EXPECT_NEAR(c.mass_balance.qual_routing_reacted[0],400,1.e-10);
}

TEST(LidNodes, PollutantRenameAndDeleteFollowLayerTreatment) {
    SWMMEngine e; e.context()=model(); auto& c=e.context(); c.state=EngineState::OPENED;
    c.pollutant_names.add("TSS"); c.pollutant_names.add("BOD"); c.pollutants.resize_pollutants(2);
    auto& rules=c.lid_controls.node_layers[0][1].treatment;
    rules={{"TSS",0,1,"C = C_TSS * 0.5"},{"BOD",0,0,"R = R_TSS"}};
    ASSERT_EQ(swmm_pollutant_rename(reinterpret_cast<SWMM_Engine>(&e),0,"Solids"),SWMM_OK);
    EXPECT_EQ(rules[0].pollutant,"Solids"); EXPECT_EQ(rules[0].expression,"C = C_Solids * 0.5");
    EXPECT_EQ(rules[1].expression,"R = R_Solids");
    edit::delete_pollutant(c,0);
    EXPECT_TRUE(rules.empty());
}

TEST(LidNodes, ChainedDrainageAndBackflowConservePollutantMass) {
    const auto dir = std::filesystem::path(OPENSWMM_TEST_SOURCE_DIR) / "../../output/lid_nodes_2026_10_04";
    std::filesystem::create_directories(dir);
    std::ifstream fixture(std::filesystem::path(OPENSWMM_TEST_SOURCE_DIR) / "data/lid_chain_mass_balance.inp");
    // OPENSWMM_TEST_SOURCE_DIR is the engine test directory.
    ASSERT_TRUE(fixture.good());
    const std::string base((std::istreambuf_iterator<char>(fixture)), {});
    for (bool media : {false, true}) for (bool backwater : {false, true}) for (bool clean_boundary : {false, true}) {
        auto deck = base;
        if(clean_boundary) deck.insert(deck.find("FLOW_UNITS"), "OUTFALL_BACKFLOW_QUALITY ZERO\n");
        if (media) {
            const auto at = deck.find("Train AGGREGATE 12 .45 100");
            ASSERT_NE(at, std::string::npos);
            deck.replace(at, std::string("Train AGGREGATE 12 .45 100").size(),
                         "Train MEDIA 12 .45 .20 .08 1 10 3");
        }
        if (!backwater) {
            auto at = deck.find("1.60");
            while (at != std::string::npos) { deck.replace(at, 4, "0.00"); at = deck.find("1.60", at + 4); }
        }
        const auto stem = std::string("chain_") + (media ? "media" : "aggregate") + (backwater ? "_backwater" : "_free") + (clean_boundary ? "_zero" : "_last");
        const auto inp = dir / (stem + ".inp");
        std::ofstream(inp) << deck;
        SWMMEngine e;
        ASSERT_EQ(e.open(inp.string().c_str(), (dir/(stem+".rpt")).string().c_str(), nullptr), 0);
        ASSERT_EQ(e.initialize(), 0); ASSERT_EQ(e.start(0), 0);
        double t=0;
        double max_relative_error = 0, max_time = 0;
        double reverse_volume = 0;
        do {
            ASSERT_EQ(e.step(&t), 0);
            const auto& c=e.context(); const auto& b=c.mass_balance;
            double stored=0;
            for(int n=0;n<c.n_nodes();++n) stored+=c.nodes.conc[n*2+1]*c.nodes.volume[n]+lidnode::heldMass(c,n,1);
            for(int j=0;j<c.n_links();++j) stored+=c.links.conc[j*2+1]*c.links.volume[j];
            const double input=b.qual_routing_init[1]+b.qual_routing_ex_in[1];
            const double output=stored+b.qual_routing_outflow[1]+b.qual_routing_flood[1]+b.qual_routing_reacted[1]+b.qual_routing_seep[1]+b.qual_routing_final_dry[1];
            if(input>1) {
                const double error=std::abs(input-output)/input;
                if(error>max_relative_error) {max_relative_error=error;max_time=t*24;}
            }
            reverse_volume+=std::max(0.0,-c.links.flow[1])*.5;
        } while(t>0);
        ASSERT_EQ(e.end(),0);
        for(const auto& warning:e.context().warnings) EXPECT_EQ(warning.find("quality mixtures did not converge"),std::string::npos) << warning;
        const auto& b=e.context().mass_balance;
        for(int p=0;p<2;++p) {
            const double input=b.qual_routing_init[p]+b.qual_routing_ex_in[p];
            const double output=b.qual_routing_final[p]+b.qual_routing_outflow[p]+b.qual_routing_flood[p]+b.qual_routing_reacted[p]+b.qual_routing_seep[p];
            EXPECT_NEAR(input,output,input*.001) << stem << " pollutant=" << p << " input=" << input << " output=" << output;
        }
        EXPECT_LT(max_relative_error,.001) << stem << " maximum tracer error at hour " << max_time;
        if(backwater) EXPECT_GT(reverse_volume,10) << stem;
        e.close();
    }
}

TEST(LidNodes, PercolationCanDrainBelowOneLitreWithoutLosingItsLoad) {
    auto c=model(); c.pollutant_names.add("Tracer");c.pollutants.resize_pollutants(1);
    c.nodes.resize_quality(1);c.nodes.conc.assign(2,10);c.nodes.conc_old.assign(2,10);
    c.mass_balance.resize_quality(1);
    lidnode::initialize(c);
    auto& state=c.node_subtypes.storages.lid_state[0];
    for(const auto& cell:state.cells)state.quality_old_water.push_back(cell.theta*cell.geometric_volume);
    const double before=10*state.held_volume;
    state.quality_old_mobile=0;
    // A tiny accepted media-to-mobile transfer drains entirely this step.
    state.quality_transfers={{5,-1,.002}};
    c.nodes.old_volume[0]=.002;c.nodes.volume[0]=0;
    c.link_names.add("Drain");c.links.resize(1);c.links.resize_quality(1);c.links.type[0]=LinkType::ORIFICE;
    c.links.node1[0]=0;c.links.node2[0]=1;c.links.flow[0]=.004;
    c.nodes.inflow[1]=.004;c.nodes.outflow[0]=.004;
    quality::QualitySolver solver;solver.init(2,1,1);solver.execute(c,.5);
    const double exported=.004*.5*c.nodes.conc[1];
    EXPECT_NEAR(exported,.02,1.e-12);
    EXPECT_NEAR(before,lidnode::heldMass(c,0,0)+exported,1.e-10);
}

TEST(LidNodes, WeirAnchorUsesItsPhysicalCrestAndTracksStackEdits) {
    auto c=model();c.link_names.add("Spill");c.links.resize(1);c.links.node1[0]=0;c.links.node2[0]=1;
    c.link_subtypes.set_link_type(c.links,0,LinkType::WEIR);
    const int wr=c.link_subtypes.weir_row(0);
    c.nodes.invert_elev[0]=10;c.nodes.invert_elev[1]=9;
    c.link_subtypes.weirs.crest_height[wr]=1.5;
    c.lid_node_outlets.push_back({0,1,false});
    lidnode::validate(c);
    EXPECT_TRUE(c.warnings.empty());
    EXPECT_DOUBLE_EQ(lidnode::portOffset(c,0,0),1.5);
    EXPECT_DOUBLE_EQ(lidnode::portOffset(c,0,1),2.5);
    c.lid_controls.node_layers[0][1].params[0]=18;
    lidnode::sync(c,0);
    EXPECT_DOUBLE_EQ(c.link_subtypes.weirs.crest_height[wr],2);
    EXPECT_DOUBLE_EQ(lidnode::portOffset(c,0,0),2);
}

TEST(LidNodes, PondedWeirAtNonzeroInvertDrawsSurfaceWater) {
    auto c=model();
    c.lid_controls.node_layers[0][2].params[0]=12;
    lidnode::sync(c,0);
    c.nodes.invert_elev[0]=.3;
    c.link_names.add("Spill");c.links.resize(1);c.links.node1[0]=0;c.links.node2[0]=1;
    c.link_subtypes.set_link_type(c.links,0,LinkType::WEIR);
    c.link_subtypes.weirs.crest_height[c.link_subtypes.weir_row(0)]=2;
    lidnode::initialize(c);
    auto& state=c.node_subtypes.storages.lid_state[0];
    state.cells.front().theta=.36; // 0.20 ft of surface ponding above the crest.
    c.nodes.depth[0]=.5; // Mobile water table remains in the aggregate.
    const double offset=lidnode::portOffset(c,0,0);
    EXPECT_NEAR(lidnode::portDepth(c,0,offset),2.2,1.e-12);
    lidnode::resetPorts(c);
    EXPECT_NEAR(lidnode::exchangePorts(c,0,.1,1),.1,1.e-12);
    EXPECT_NEAR(state.port_delta.front(),-.1,1.e-12);
    for(std::size_t i=1;i<state.port_delta.size();++i) EXPECT_EQ(state.port_delta[i],0);
}

TEST(LidNodes, PortInterfacesAllowRoundoffButPreservePhysicalOffsets) {
    auto c=model();lidnode::initialize(c);
    const auto& state=c.node_subtypes.storages.lid_state[0];
    for(std::size_t i=0;i+1<state.cells.size();++i) {
        const double boundary=state.cells[i].bottom;
        EXPECT_EQ(lidnode::portCell(state,boundary),i);
        EXPECT_EQ(lidnode::portCell(state,std::nextafter(boundary,0.0)),i);
        EXPECT_EQ(lidnode::portCell(state,std::nextafter(boundary,std::numeric_limits<double>::infinity())),i);
        EXPECT_EQ(lidnode::portCell(state,boundary-1.e-8),i+1);
    }
}

TEST(LidNodes, MobileOutletTreatmentUsesTheSameInterfaceOwnerAsHydraulics) {
    auto c=model();
    c.pollutant_names.add("Tracer");c.pollutants.resize_pollutants(1);
    c.nodes.resize_quality(1);c.nodes.conc.assign(2,10);c.nodes.conc_old.assign(2,10);
    c.mass_balance.resize_quality(1);
    // A port exactly at the media/aggregate interface belongs to MEDIA.
    c.lid_controls.node_layers[0][1].treatment={{"Tracer",.25,0,{}}};
    c.lid_controls.node_layers[0][2].treatment={{"Tracer",.75,0,{}}};
    c.link_names.add("Drain");
    c.links.resize(1);c.links.resize_quality(1);c.links.node1[0]=0;c.links.node2[0]=1;
    c.links.flow[0]=.1;
    lidnode::initialize(c);
    for(double offset:{.5,std::nextafter(.5,0.0),std::nextafter(.5,1.0)}) {
        c.links.offset1[0]=offset;
        c.node_subtypes.storages.lid_state[0].quality_outlet_conc.assign(1,std::numeric_limits<double>::quiet_NaN());
        lidnode::prepareOutletQuality(c,1,false);
        EXPECT_NEAR(lidnode::outletQuality(c,0,0,0,10),7.5,1.e-12);
    }
    c.links.offset1[0]=.5-1.e-8;
    lidnode::prepareOutletQuality(c,1,false);
    EXPECT_NEAR(lidnode::outletQuality(c,0,0,0,10),2.5,1.e-12);
}
