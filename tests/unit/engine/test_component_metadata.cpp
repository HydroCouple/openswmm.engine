// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <filesystem>
#include <fstream>
namespace fs=std::filesystem;
namespace {
struct Engine {
    SWMM_Engine value=swmm_engine_create();
    ~Engine(){swmm_engine_destroy(value);}
};
fs::path fixture(const std::string& name,bool declaredOutputs=false) {
    auto directory=fs::absolute(fs::path("component_metadata")/name);
    fs::create_directories(directory);
    fs::copy_file("component_metadata/reference.inp",directory/"model.inp",fs::copy_options::overwrite_existing);
    if(declaredOutputs) {
        std::ofstream f(directory/"model.inp",std::ios::app);
        f<<"\n[FILES]\nSAVE OUTFLOWS \""<<(directory/"declared.iface").string()<<"\"\n"
         <<"SAVE HOTSTART \""<<(directory/"declared.hsf").string()<<"\"\n"
         <<"[PLUGINS]\nmissing_metadata_probe_plugin\n";
    }
    return directory;
}
}
TEST(ComponentMetadata, ParsedCatalogueWithoutRuntimePluginsOrOutputFiles) {
    const auto directory=fixture("discovery",true); Engine e;
    ASSERT_EQ(swmm_engine_open_metadata(e.value,(directory/"model.inp").string().c_str()),SWMM_OK);
    EXPECT_EQ(swmm_node_count(e.value),2); EXPECT_EQ(swmm_link_count(e.value),1); EXPECT_EQ(swmm_pollutant_count(e.value),2);
    EXPECT_STREQ(swmm_pollutant_id(e.value,0),"A");
    int units=-1; ASSERT_EQ(swmm_pollutant_get_units(e.value,1,&units),0); EXPECT_EQ(units,1);
    int plugins=0; ASSERT_EQ(swmm_plugins_count(e.value,&plugins),0); EXPECT_EQ(plugins,1);
    for(int i=0;i<swmm_get_warning_count(e.value);++i)
        EXPECT_EQ(std::string(swmm_get_warning_at(e.value,i)).find("missing_metadata_probe_plugin"),std::string::npos);
    double elapsed=-1; ASSERT_EQ(swmm_engine_get_elapsed_seconds(e.value,&elapsed),0); EXPECT_DOUBLE_EQ(elapsed,0);
    ASSERT_EQ(swmm_engine_close(e.value),0);
    EXPECT_FALSE(fs::exists(directory/"declared.iface")); EXPECT_FALSE(fs::exists(directory/"declared.hsf"));
    for(const auto& entry:fs::directory_iterator(directory))EXPECT_EQ(entry.path().filename(),"model.inp");
}
TEST(ComponentMetadata, CannotInitializeOrStartAndCanReopenNormally) {
    const auto directory=fixture("reopen"); Engine e;
    ASSERT_EQ(swmm_engine_open_metadata(e.value,(directory/"model.inp").string().c_str()),0);
    EXPECT_EQ(swmm_engine_initialize(e.value),SWMM_ERR_LIFECYCLE);
    EXPECT_NE(swmm_engine_start(e.value,1),0);
    ASSERT_EQ(swmm_engine_close(e.value),0);
    ASSERT_EQ(swmm_engine_open(e.value,(directory/"model.inp").string().c_str(),nullptr,nullptr,nullptr),0);
    ASSERT_EQ(swmm_engine_initialize(e.value),0); ASSERT_EQ(swmm_engine_start(e.value,0),0);
    double actual=0; ASSERT_EQ(swmm_engine_advance_to(e.value,60,&actual),0); EXPECT_DOUBLE_EQ(actual,60);
}
TEST(ComponentMetadata, ReactionMetadataUsesTheExistingRegistry) {
    const auto directory=fixture("reactions");
    std::ofstream(directory/"model.rxn")<<"[REACTION_SPECIES]\nBULK X MG\nWALL W MG\n[REACTION_PIPES]\nFORMULA X 2*A\n";
    std::ofstream(directory/"model.inp",std::ios::app)<<"\n[PROCESS_COMPONENTS]\norg.hydrocouple.openswmm.reactions config=\"model.rxn\"\n";
    Engine e; ASSERT_EQ(swmm_engine_open_metadata(e.value,(directory/"model.inp").string().c_str()),0);
    ASSERT_EQ(swmm_reaction_species_count(e.value),2);
    char name[64],units[64]; int wall=-1; double atol,rtol;
    ASSERT_EQ(swmm_reaction_species_get(e.value,1,name,64,&wall,units,64,&atol,&rtol),0);
    EXPECT_STREQ(name,"W"); EXPECT_STREQ(units,"MG"); EXPECT_EQ(wall,1);
}
TEST(ComponentMetadata, ValidatesHandlesPathsAndStrictInputEvenAfterLenientSelection) {
    Engine e; EXPECT_EQ(swmm_engine_open_metadata(nullptr,"x"),SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_engine_open_metadata(e.value,nullptr),SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_engine_open_metadata(e.value,""),SWMM_ERR_BADPARAM);
    const auto directory=fixture("invalid"); std::ofstream(directory/"bad.inp")<<"[OPTIONS]\nFLOW_UNITS CFS\n[JUNCTIONS]\nJ 10 10\n[CONDUITS]\nC J MISSING 10 .013 0 0\n[XSECTIONS]\nC CIRCULAR 2\n";
    swmm_engine_set_lenient_open(e.value,1);
    EXPECT_NE(swmm_engine_open_metadata(e.value,(directory/"bad.inp").string().c_str()),0);
}
