// SPDX-License-Identifier: Apache-2.0
#include <openswmm/engine/openswmm_split_material.h>
#include "SWMMEngine.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <memory>
#include <new>
#include <string_view>

struct SWMM_MaterialCheckpointOpaque {
    explicit SWMM_MaterialCheckpointOpaque(std::string value):bytes(std::move(value)){}
    const std::string bytes;
};
namespace {
thread_local char diagnostic[2048]{};
int failure(int code,std::string_view text) noexcept {
    auto n=std::min(text.size(),sizeof diagnostic-1);
    if(n<text.size())while(n && (static_cast<unsigned char>(text[n])&0xc0)==0x80)--n;
    std::memcpy(diagnostic,text.data(),n);diagnostic[n]=0;return code;
}
template<class F> int operation(F&& call) noexcept {
    diagnostic[0]=0;
    try{return call();}
    catch(const std::bad_alloc&){return failure(SWMM_ERR_NOMEM,"Cannot allocate material checkpoint operation.");}
    catch(const std::exception& e){return failure(SWMM_ERR_INTERNAL,e.what());}
    catch(...){return failure(SWMM_ERR_INTERNAL,"Non-standard material checkpoint exception.");}
}
int unavailable() noexcept {return failure(SWMM_ERR_PLUGIN,"Material checkpoint support is unavailable in this build.");}
using Engine=openswmm::SWMMEngine;
int ready(Engine& engine,bool attached=true) noexcept {
    return engine.materialPairCheckpointAccessReady(attached)?SWMM_OK:
        failure(SWMM_ERR_LIFECYCLE,"Material recovery requires its idle healthy bound network/surface pair.");
}
}
extern "C" {
const char* swmm_split_material_checkpoint_last_error(void){return diagnostic;}
int swmm_split_material_checkpoint_capabilities(SWMM_Engine engine,unsigned long* flags) {
    return operation([&]() -> int {
        if(!engine)return failure(SWMM_ERR_BADHANDLE,"Null material checkpoint engine.");
        if(!flags)return failure(SWMM_ERR_BADPARAM,"Null material checkpoint flags.");
        unsigned long value=0;
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
        value=SWMM_MATERIAL_CHECKPOINT_NUMERICAL;
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
        value|=SWMM_MATERIAL_CHECKPOINT_OUTPUTS;
#endif
#endif
        *flags=value;return SWMM_OK;
    });
}
int swmm_split_material_checkpoint_prepare_outputs(SWMM_Engine handle) {
    return operation([&]() -> int {
        if(!handle)return failure(SWMM_ERR_BADHANDLE,"Null material checkpoint engine.");
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D) && defined(OPENSWMM_HAS_2D_HDF5_OUTPUT)
        const auto error=static_cast<Engine*>(handle)->prepareMaterialPairOutputCheckpointing();
        return error.empty()?SWMM_OK:failure(SWMM_ERR_LIFECYCLE,error);
#else
        return unavailable();
#endif
    });
}
int swmm_split_material_checkpoint_enable(SWMM_Engine handle,int outputs) {
    return operation([&]() -> int {
        if(!handle)return failure(SWMM_ERR_BADHANDLE,"Null material checkpoint engine.");
        if(outputs!=0 && outputs!=1)return failure(SWMM_ERR_BADPARAM,"Material checkpoint outputs must be 0 or 1.");
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
#ifndef OPENSWMM_HAS_2D_HDF5_OUTPUT
        if(outputs)return unavailable();
#endif
        auto& engine=*static_cast<Engine*>(handle);const int rc=ready(engine,false);if(rc)return rc;
        const auto error=engine.enableMaterialPairCheckpoint(outputs!=0);
        return error.empty()?SWMM_OK:failure(SWMM_ERR_LIFECYCLE,error);
#else
        return unavailable();
#endif
    });
}
int swmm_split_material_checkpoint_capture(SWMM_Engine handle,SWMM_MaterialCheckpoint* snapshot) {
    return operation([&]() -> int {
        if(!handle)return failure(SWMM_ERR_BADHANDLE,"Null material checkpoint engine.");
        if(!snapshot)return failure(SWMM_ERR_BADPARAM,"Null material checkpoint result.");
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
        auto& engine=*static_cast<Engine*>(handle);const int rc=ready(engine);if(rc)return rc;
        std::string bytes;const auto error=engine.materialPairCheckpointBytes(bytes);
        if(!error.empty())return failure(SWMM_ERR_IO,error);
        auto result=std::make_unique<SWMM_MaterialCheckpointOpaque>(std::move(bytes));
        *snapshot=result.release();return SWMM_OK;
#else
        return unavailable();
#endif
    });
}
int swmm_split_material_checkpoint_from_bytes(const void* bytes,std::size_t size,SWMM_MaterialCheckpoint* snapshot) {
    return operation([&]() -> int {
        if(!bytes || !size || size>SWMM_MATERIAL_CHECKPOINT_MAX_BYTES || !snapshot)
            return failure(SWMM_ERR_BADPARAM,"Invalid material checkpoint binary input/size/result.");
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
        std::string value(static_cast<const char*>(bytes),size);
        const auto error=Engine::validateMaterialPairCheckpointEnvelope(value);
        if(!error.empty())return failure(SWMM_ERR_BADPARAM,error);
        auto result=std::make_unique<SWMM_MaterialCheckpointOpaque>(std::move(value));
        *snapshot=result.release();return SWMM_OK;
#else
        return unavailable();
#endif
    });
}
int swmm_split_material_checkpoint_data(SWMM_MaterialCheckpoint snapshot,const void** bytes,std::size_t* size) {
    return operation([&]() -> int {
        if(!snapshot)return failure(SWMM_ERR_BADHANDLE,"Null material checkpoint snapshot.");
        if(!bytes || !size)return failure(SWMM_ERR_BADPARAM,"Null material checkpoint data/length output.");
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
        *bytes=snapshot->bytes.data();*size=snapshot->bytes.size();return SWMM_OK;
#else
        return unavailable();
#endif
    });
}
int swmm_split_material_checkpoint_restore(SWMM_Engine handle,SWMM_MaterialCheckpoint snapshot) {
    return operation([&]() -> int {
        if(!handle || !snapshot)return failure(SWMM_ERR_BADHANDLE,"Null material checkpoint engine/snapshot.");
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
        auto& engine=*static_cast<Engine*>(handle);const int rc=ready(engine);if(rc)return rc;
        if(engine.context().elapsed_ms!=0)return failure(SWMM_ERR_LIFECYCLE,"Material restore requires two fresh running targets.");
        const auto error=engine.restoreMaterialPairCheckpoint(snapshot->bytes);
        return error.empty()?SWMM_OK:failure(SWMM_ERR_IO,error);
#else
        return unavailable();
#endif
    });
}
int swmm_split_material_checkpoint_restore_at(SWMM_Engine handle,SWMM_MaterialCheckpoint snapshot,double expected_seconds) {
    return operation([&]() -> int {
        if(!handle || !snapshot)return failure(SWMM_ERR_BADHANDLE,"Null material checkpoint engine/snapshot.");
        if(!std::isfinite(expected_seconds) || expected_seconds<0)
            return failure(SWMM_ERR_BADPARAM,"Expected material accepted seconds must be finite and nonnegative.");
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
        auto& engine=*static_cast<Engine*>(handle);const int rc=ready(engine);if(rc)return rc;
        if(engine.context().elapsed_ms!=0)return failure(SWMM_ERR_LIFECYCLE,"Material restore requires two fresh running targets.");
        const auto error=engine.restoreMaterialPairCheckpoint(snapshot->bytes,expected_seconds);
        return error.empty()?SWMM_OK:failure(SWMM_ERR_IO,error);
#else
        return unavailable();
#endif
    });
}
void swmm_split_material_checkpoint_destroy(SWMM_MaterialCheckpoint snapshot){delete snapshot;}
}
