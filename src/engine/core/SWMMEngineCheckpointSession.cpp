// SPDX-License-Identifier: Apache-2.0
#include "SWMMEngine.hpp"
#include "CheckpointInspection.hpp"
#include "ReactionCheckpoint.hpp"
#include "FileIO.hpp"
#include "../plugins/DefaultInputPlugin.hpp"
#include "../plugins/DefaultReportPlugin.hpp"
#include "../plugins/DefaultStateIOPlugin.hpp"
#include <openswmm/engine/openswmm_checkpoint.h>
#ifdef OPENSWMM_HAS_2D
#include "../2d/subsurface/CommonSourceStage.hpp"
#include "../2d/subsurface/EngineSourceResultStream.hpp"
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
#include "../2d/output/Default2DOutputPlugin.hpp"
#endif
#endif
#include <filesystem>
#include <algorithm>
#include <stdexcept>

namespace openswmm {
struct CheckpointSessionResources {
#ifdef OPENSWMM_HAS_2D
    runoff::SourceWaterDriver sources;
    twoD::CommonSourceStage stage;
    twoD::EngineSourceResultStream stream;
#endif
};
namespace {
void require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
void checked(const std::string& error){require(error.empty(),error);}
void freshOutputs(const std::vector<std::string>& paths) {
    std::vector<std::filesystem::path> normalized;
    for(const auto& path:paths)if(!path.empty()) {
        std::error_code ec;const auto file=io::utf8_path(path);const auto status=std::filesystem::symlink_status(file,ec);
        require((!ec||ec==std::errc::no_such_file_or_directory)&&!std::filesystem::exists(status),"Managed checkpoint output destination already exists or is unreadable.");
        const auto canonical=std::filesystem::weakly_canonical(file);
        for(const auto& prior:normalized)require(prior!=canonical,"Managed checkpoint output destinations alias one another.");
        normalized.push_back(canonical);
    }
}
#ifdef OPENSWMM_HAS_2D
void reviewed(const twoD::SurfaceOwnershipPreview& preview) {
    require(!preview.cancelled,"Ownership preview was cancelled.");
    require(preview.valid(),preview.errors.empty()?"Invalid source ownership.":preview.errors.front());
}
#endif
}
std::string SWMMEngine::startCheckpointSession(int output,const std::string& source_path,
    bool detailed,const std::string& token) {
    bool initializing=false;
    bool complete=false;
    struct FailureClose {
        SWMMEngine& engine;bool& initializing;bool& complete;
        ~FailureClose(){if(initializing&&!complete)engine.close();}
    } failure_close{*this,initializing,complete};
    try {
        require(ctx_.state==EngineState::OPENED&&!step_active_&&!runtime_pipeline_&&!checkpoint_enabled_,
            "Start the managed checkpoint session from OPENED.");
        require(output>=0&&output<=SWMM_CHECKPOINT_OUTPUT_FILES,"Invalid checkpoint output profile.");
        require(!detailed||!source_path.empty(),"Detailed source recording requires a journal.");
        require(output!=SWMM_CHECKPOINT_OUTPUT_FILES||!source_path.empty(),"File companions require a source journal.");
        require(source_path.find('\0')==std::string::npos&&detail::checkpointUtf8(source_path),"Invalid source journal path.");
        require(!callbacks_.on_progress&&!callbacks_.on_warning&&!callbacks_.on_step_begin&&!callbacks_.on_step_end,
            "Managed checkpoint sessions cannot recover callbacks.");
        require(ctx_.plugin_specs.empty()&&detail::checkpointReactionComponents(ctx_),"Managed checkpoint sessions require native inputs and writers.");
        require(plugins_.input_plugins().size()==1&&dynamic_cast<DefaultInputPlugin*>(plugins_.input_plugins().front()),
            "Managed checkpoint sessions require the built-in input reader.");
        require(plugins_.state_io_plugins().size()==1&&dynamic_cast<DefaultStateIOPlugin*>(plugins_.state_io_plugins().front())&&
            plugins_.report_plugins().size()==(rpt_path_.empty()?0u:1u)&&
            (rpt_path_.empty()||dynamic_cast<DefaultReportPlugin*>(plugins_.report_plugins().front())),
            "Managed checkpoint sessions require native state and report writers.");
        // Refuse ancillary writers before initialize/start can touch their paths.
        require(!ctx_.files.has_any(),"Managed checkpoint sessions cannot recover interface or hotstart files.");
        for(const auto& path:ctx_.lid_usage.rpt_file)require(path.empty(),"Managed checkpoint sessions cannot append detailed LID reports.");
        bool mesh_quality=false;
#ifdef OPENSWMM_HAS_2D
        mesh_quality=!ctx_.options.ignore_2d&&surface_router_.mesh().n_cells()>0&&ctx_.n_nodes()==0&&
            ctx_.n_links()==0&&ctx_.n_subcatches()==0&&surface_router_.aquiferConfig().surface_owners.empty()&&
            (surface_router_.options().reconstruction_order==1||surface_router_.options().reconstruction_order==2)&&
            (ctx_.reactions.configured||ctx_.options.water_age||ctx_.options.heat_transport||
             (!ctx_.options.ignore_quality&&ctx_.n_pollutants()>0));
#endif
        require(mesh_quality||(!ctx_.options.heat_transport&&!ctx_.options.water_age&&(ctx_.options.ignore_quality||ctx_.n_pollutants()==0)),
            "Managed checkpoint sessions require water-only, supported 1D reactions or mesh-only matched ground quality.");
        require(!mesh_quality||output==SWMM_CHECKPOINT_OUTPUT_NONE||output==SWMM_CHECKPOINT_OUTPUT_MESH,
            "Mesh-only matched ground quality sessions support numerical state or mesh output; combined/binary companions remain guarded.");
#ifndef OPENSWMM_HAS_2D_HDF5_OUTPUT
        require(output<SWMM_CHECKPOINT_OUTPUT_MESH,"Mesh output checkpoints unavailable in this build.");
#endif
        const bool binary=output==SWMM_CHECKPOINT_OUTPUT_BINARY||output>=SWMM_CHECKPOINT_OUTPUT_COMBINED;
        require(!binary||!out_path_.empty(),"Binary checkpoint output requires its fresh output path.");
        require(binary||out_path_.empty(),"Open without a binary destination for a profile without binary output.");
        std::vector<std::string> outputs{rpt_path_,binary?out_path_:"",source_path};
#ifdef OPENSWMM_HAS_2D
        auto& r=surface_router_;const auto& owners=r.aquiferConfig().surface_owners;
        const bool surface=!ctx_.options.ignore_2d&&r.mesh().n_cells()>0;
        require(surface||owners.empty(),"Reviewed source ownership requires an active mesh.");
        require(surface||(source_path.empty()&&output<=SWMM_CHECKPOINT_OUTPUT_BINARY),"Surface recording requires an active mesh.");
        require(output<SWMM_CHECKPOINT_OUTPUT_MESH||!r.options().output_file.empty(),"Mesh output requires its fresh output path.");
        require(output<SWMM_CHECKPOINT_OUTPUT_MESH||io::utf8_path(r.options().output_file).is_absolute(),"Managed mesh output requires an absolute path.");
        require(output>=SWMM_CHECKPOINT_OUTPUT_MESH||r.options().output_file.empty(),"Clear the mesh output path for a profile without mesh output.");
        require(output>=SWMM_CHECKPOINT_OUTPUT_MESH||!surface_output_plugin_,"Open without a mesh writer for a profile without mesh output.");
        if(output>=SWMM_CHECKPOINT_OUTPUT_MESH)outputs.push_back(r.options().output_file);
        if(surface) {
            require(!r.aquiferConfig().empty()&&r.options().groundwater!=0&&!r.aquiferConfig().options.per_subcatch,
                "Managed surface sessions require the mesh aquifer/common-source profile.");
            if(r.options().reconstruction_order==2)
                require(ctx_.n_subcatches()==0&&owners.empty()&&source_path.empty(),
                    "Managed RK2 requires the mesh/aquifer owner without catchment or source journal; any network must have no shared exchanges.");
            if(!owners.empty()) {
                const auto preview=twoD::resolveSurfaceOwnership(ctx_,r.mesh(),r.options(),r.aquiferConfig(),owners);
                reviewed(preview);require(!token.empty()&&token==preview.token,"Reviewed ownership token is stale; preview the current model again.");
            } else require(token.empty(),"Ownership token supplied without ownership records.");
            const bool lid_beds=std::any_of(ctx_.node_subtypes.storages.lid.begin(),ctx_.node_subtypes.storages.lid.end(),
                [](const auto& l){return l.control>=0 && l.bottom==LidNodeBottom::MeshAquifer;});
            require(source_path.empty()||!owners.empty()||lid_beds,"Journals require reviewed catchment or LID bottom ownership.");
        } else require(token.empty(),"Ownership token supplied for a 1D session.");
#else
        require(source_path.empty()&&token.empty(),"Surface checkpoint session unavailable in this build.");
#endif
        freshOutputs(outputs);
        const size_t native_writers=(binary_output_plugin_?1u:0u)
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
            +(surface_output_plugin_?1u:0u)
#endif
            ;
        require(plugins_.output_plugins().size()==native_writers,"Managed checkpoint sessions require native output writers.");
        auto resources=std::make_shared<CheckpointSessionResources>();
        initializing=true;
        if(!rpt_path_.empty())static_cast<DefaultReportPlugin*>(plugins_.report_plugins().front())->requireFreshOutput();
        // File-slot edits in OPENED change configuration, while open() has
        // already registered its writer. Bind only the reviewed fresh path.
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
        if(output>=SWMM_CHECKPOINT_OUTPUT_MESH) {
            if(surface_output_plugin_)surface_output_plugin_->h5_path_=r.options().output_file;
            else {
                auto writer=std::make_unique<twoD::Default2DOutputPlugin>(r.options().output_file);
                require(writer->initialize({},nullptr)==0&&writer->validate(ctx_)==0,"Cannot prepare the native mesh writer.");
                try {plugins_.add_output_plugin(writer.get());}
                catch(...) {
                    // The factory can append the pointer before allocating its
                    // argument row. Let close own that already registered object.
                    if(std::find(plugins_.output_plugins().begin(),plugins_.output_plugins().end(),writer.get())!=plugins_.output_plugins().end())writer.release();
                    throw;
                }
                surface_output_plugin_=writer.release();
            }
        }
#endif
#ifdef OPENSWMM_HAS_2D
        // This opt-in only covers initialize; ordinary runs retain the guard.
        int initialized;
        {struct ResetOptIn {bool& flag;~ResetOptIn(){flag=false;}} reset{r.checkpoint_owned_initialization_};
         r.checkpoint_owned_initialization_=!owners.empty();initialized=initialize();}
#else
        const auto initialized=initialize();
#endif
        require(initialized==SWMM_OK,ctx_.error_message);
#ifdef OPENSWMM_HAS_2D
        if(mesh_quality&&r.options().reconstruction_order==1)require(twoD::CommonSourceStage::supportsMeshGroundQuality(ctx_,r.state(),r.subsurface()),
            "Managed mesh quality requires matched bulk-species layouts and either WATER_ONLY without heat or paired porous-ground thermal owners.");
        if(output>=SWMM_CHECKPOINT_OUTPUT_COMBINED)checked(prepareCombinedOutputCheckpointing());
        else if(output==SWMM_CHECKPOINT_OUTPUT_BINARY)checked(prepareOutputCheckpointing());
        else if(output==SWMM_CHECKPOINT_OUTPUT_MESH)checked(prepareSurfaceOutputCheckpointing());
#else
        if(binary)checked(prepareOutputCheckpointing());
#endif
        require(start(binary?1:0)==SWMM_OK,ctx_.error_message);
        // Own before non-owning attachments are installed, including failures.
        checkpoint_session_resources_=resources;
#ifdef OPENSWMM_HAS_2D
        if(r.isActive()&&r.options().reconstruction_order==2) {
            checked(enableCheckpointing());
        } else if(r.isActive()) {
            twoD::SurfaceOwnershipPreview preview;std::vector<int> seeds;std::vector<std::pair<int,double>> areas;
            const bool spatial=!owners.empty();
            if(spatial) {
                // Initialization normalizes mesh units; recompute physical SI
                // shares with the same authoritative geometry/area resolver.
                preview=twoD::resolveSurfaceOwnership(ctx_,r.mesh(),r.options(),r.aquiferConfig(),owners);reviewed(preview);
                for(const auto& owner:owners)seeds.push_back(ctx_.subcatch_names.find(owner.subcatch));
                // The authoring resolver uses exact SI footprints. Supply its
                // non-LID areas to avoid the legacy rounded LANDAREA denominator.
                for(const auto& object:preview.objects)areas.emplace_back(object.subcatch,object.declared_area-object.lid_area);
            } else preview.mesh_weather_area=r.mesh().tri_area;
            // Initial receipt mode is immutable; it closes native outfall returns
            // into owned graphs without changing the authored routing topology.
            const bool returns=spatial;
            checked(resources->sources.initialize(ctx_,seeds,areas,&lid_,returns));
            checked(resources->stage.initialize(r.mesh(),r.state(),r.options(),r.subsurface(),resources->sources,preview,!spatial));
            if(spatial)checked(enableSpatialSourceCheckpointing(resources->stage));
            else checked(enableCheckpointing(resources->stage));
            checkpoint_session_resources_=resources;
            if(!source_path.empty())checked(setEngineSourceResultStream(resources->stream,source_path,detailed));
            if(output==SWMM_CHECKPOINT_OUTPUT_FILES)checked(enableFileOutputCheckpointing());
        } else
#endif
        checked(enableCheckpointing());
        checkpoint_session_resources_=resources;
        complete=true;return {};
    } catch(const std::exception& e) {
        const std::string error=e.what();
#ifdef OPENSWMM_HAS_2D
        surface_router_.checkpoint_owned_initialization_=false;
#endif
        return error;
    } catch(...) {
#ifdef OPENSWMM_HAS_2D
        surface_router_.checkpoint_owned_initialization_=false;
#endif
        throw;
    }
}

std::string SWMMEngine::prepareNativeHotstartCheckpointing() {
    try {
        require(ctx_.state==EngineState::RUNNING && ctx_.current_time==0 && !checkpoint_enabled_,
                "Prepare native hotstart checkpointing at the initial running boundary.");
#ifdef OPENSWMM_HAS_2D
        if(surface_router_.isActive()) {
            auto resources=std::make_shared<CheckpointSessionResources>();
            auto& router=surface_router_;
            checked(resources->sources.initialize(ctx_,{},{},&lid_,false));
            twoD::SurfaceOwnershipPreview preview;
            preview.mesh_weather_area=router.mesh().tri_area;
            checked(resources->stage.initialize(router.mesh(),router.state(),router.options(),
                router.subsurface(),resources->sources,preview,true));
            checked(enableCheckpointing(resources->stage));
            checkpoint_session_resources_=std::move(resources);
            return {};
        }
#endif
        checked(enableCheckpointing());
        return {};
    } catch(const std::exception& e) {return e.what();}
}
}
