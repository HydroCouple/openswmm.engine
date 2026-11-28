// SPDX-License-Identifier: Apache-2.0
/**
 * @file      WebGpuSurfaceSolver.cpp
 * @brief     WebGPU surface marcher: GPU thread, buffers, and the per-advance contract.
 *
 * @details See WebGpuSurfaceSolver.hpp for the scope and WebGpuKernels.hpp for the kernels and
 *          the precision contract. Every WebGPU call runs on the GPU thread inside
 *          Device::run(); the engine thread is blocked meanwhile, so the lambdas below read and
 *          write the solver's members without further synchronization.
 *
 * @see ExplicitKokkosSurfaceSolver.cpp (the native GPU marcher this follows)
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "WebGpuSurfaceSolver.hpp"

#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <webgpu/webgpu.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>

#include "../../data/NodeData.hpp"
#include "../coupling/NodeCoupling.hpp"
#include "../data/BoundaryData.hpp"
#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../solver/InertialKernels.hpp"

// True when this worker exposes WebGPU (navigator.gpu); checked before any wgpu call, which
// would otherwise throw inside JavaScript. The body is JavaScript, so clang-format stays out.
// clang-format off
EM_JS(int, openswmm_webgpu_present, (), {
    return (typeof navigator !== "undefined" && navigator.gpu) ? 1 : 0;
});
// clang-format on

namespace openswmm::twoD::webgpu {

namespace {

/// Kernels in WebGpuKernels.hpp, by entry point.
enum class Pipe : int {
    Plan,
    FinishPlan,
    Sync,
    Halo,
    Ring,
    Frontier,
    Cfl,
    TierAssign,
    Pull,
    TierCount,
    FaceTier,
    Control,
    FinishControl,
    Faces,
    Cells,
    Boundary,
    AdvanceClock,
    ReconstructAll,
    CloseFaces,
    RefreshPerot,
    RefreshBoundaryPerot,
    PublishFlux,
    Exchange,
    RkSnapshot,
    RkAverage,
    VertexBounds,
    Gradients,
    PhReset,
    PhSave,
    PhRestore,
    PhRotate,
    PhWallDrag,
    PhConnected,
    PhStencil,
    PhSingle,
    PhFaceStress,
    PhGather,
    PhPredict,
    PhCorrect,
    PhAccept,
    PhEnergyPart,
    PhEnergySum,
    PhWaveCfl,
    PhSingleFlags,
    PhChain,
    SpFold,
    Count
};
constexpr int kPipeCount = static_cast<int>(Pipe::Count);
constexpr const char* kEntry[kPipeCount] = {
    "plan",          "finishPlan",    "sync",
    "halo",          "ring",          "frontier",
    "cfl",           "tierAssign",    "pull",
    "tierCount",     "faceTier",      "control",
    "finishControl", "faces",         "cells",
    "boundary",      "advanceClock",  "reconstructAll",
    "closeFaces",    "refreshPerot",  "refreshBoundaryPerot",
    "publishFlux",   "exchange",      "rkSnapshot",
    "rkAverage",     "vertexBounds",  "gradients",
    "phReset",       "phSave",        "phRestore",
    "phRotate",      "phWallDrag",    "phConnected",
    "phStencil",     "phSingle",      "phFaceStress",
    "phGather",      "phPredict",     "phCorrect",
    "phAccept",      "phEnergyPart",  "phEnergySum",
    "phWaveCfl",     "phSingleFlags", "phChain",
    "spFold"};

/// Base substeps per macro cycle at most. The group-1 table holds one entry per substep and
/// selector S.y: 0 every tier, 1 tier 0, 2 the slower tiers (cells); 1 copy, 2 widen (FRONT_REBUILD
/// rings); 1, 2 the Jacobi direction (tier pull-down); 3 evaluate, 4 apply (RK2 exchange passes).
constexpr std::uint32_t kSelectors = 5;
constexpr std::uint32_t kSubsteps = 1u << (kMaxTiers - 1);
static_assert(kChainRounds + 1 < kSubsteps, "the corner rounds ride on the substep table");

/// 2^30 volume units per m3 (WebGpuKernels.hpp).
constexpr int kUnitExponent = 30;
/// Momentum physics: a mixing sub-step is accepted when the kinetic energy rises by at most this
/// fraction (the FP64 marchers use 1e-13; f32 momentum and sums resolve about 1e-7, so the
/// owner chose an f32-appropriate tolerance, 2026-11-23).
constexpr double kMixingEnergyTolerance = 1e-5;

WGPUStringView view(const char* s) {
    return WGPUStringView{s, WGPU_STRLEN};
}
std::string text(WGPUStringView s) {
    if (!s.data)
        return {};
    return s.length == WGPU_STRLEN ? std::string(s.data) : std::string(s.data, s.length);
}

std::int64_t toUnits(double m3) {
    return std::llround(std::ldexp(m3, kUnitExponent));
}
double fromUnits(std::int64_t units) {
    return std::ldexp(static_cast<double>(units), -kUnitExponent);
}

void* gpuMain(void* started) {
    static_cast<std::atomic<int>*>(started)->store(1);
    // Keep the thread alive with its event loop running: proxied work and WebGPU callbacks are
    // both delivered there.
    emscripten_exit_with_live_runtime();
    return nullptr;
}

} // namespace

/// Called on the GPU thread; must eventually call its argument exactly once.
using Done = std::function<void()>;
using Work = std::function<void(const Done&)>;

/**
 * @brief The GPU thread and the WebGPU device, pipelines and bind group layout (one per module).
 */
class Device {
public:
    /// The process's device, created on first use; nullptr with @p why when unavailable.
    static Device* get(std::string& why) {
        static std::once_flag once;
        static Device* device = nullptr;
        static std::string reason;
        std::call_once(once, [] {
            // Kept for the life of the module, also on failure: a GPU thread that started late
            // still points at it while it idles.
            auto* d = new Device();
            if (d->start(reason))
                device = d;
        });
        if (!device)
            why = reason;
        return device;
    }

    /**
     * @brief Run @p work on the GPU thread and block until it calls done().
     * @throws std::runtime_error The GPU thread is gone, or WebGPU reported an error.
     */
    void run(const Work& work) {
        if (!emscripten_proxy_sync_with_ctx(emscripten_proxy_get_system_queue(), thread_,
                                            &Device::trampoline, const_cast<Work*>(&work)))
            throw std::runtime_error("WebGPU 2D: the GPU thread is not running");
        if (!error_.empty()) {
            std::string e = std::move(error_);
            error_.clear();
            throw std::runtime_error("WebGPU 2D: " + e);
        }
    }

    /// Record the first WebGPU error until run() reports it (GPU thread).
    void fail(const std::string& message) {
        if (error_.empty())
            error_ = message;
    }

    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPUComputePipeline pipe[kPipeCount] = {};
    /// Group 1: the base-substep index s and the cells filter c at dynamic offset
    /// subOffset(s, c).
    WGPUBindGroup sub_group = nullptr;
    std::uint32_t sub_stride = 256;
    std::uint32_t subOffset(std::uint32_t s, std::uint32_t filter = 0) const {
        return (filter * kSubsteps + s) * sub_stride;
    }

    /// Begin a compute pass with both bind groups set (substep 0).
    WGPUComputePassEncoder beginPass(WGPUCommandEncoder enc, WGPUBindGroup group) const {
        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
        wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
        const std::uint32_t zero = 0;
        wgpuComputePassEncoderSetBindGroup(pass, 1, sub_group, 1, &zero);
        return pass;
    }
    WGPULimits limits = WGPU_LIMITS_INIT;

private:
    static void trampoline(em_proxying_ctx* ctx, void* arg) {
        (*static_cast<Work*>(arg))([ctx] { emscripten_proxy_finish(ctx); });
    }

    bool start(std::string& why) {
        std::atomic<int> started{0};
        if (pthread_create(&thread_, nullptr, gpuMain, &started) != 0) {
            why = "the GPU thread could not be created";
            return false;
        }
        // A thread pool exhausted by OpenMP would make the thread start only after this one
        // yields; wait a bounded time instead of blocking on it.
        for (int k = 0; k < 500 && !started.load(); ++k)
            emscripten_thread_sleep(10);
        if (!started.load()) {
            why = "the GPU thread did not start (thread pool exhausted)";
            return false;
        }
        std::string status;
        try {
            run([&](const Done& done) { requestDevice(status, done); });
        } catch (const std::exception& e) {
            status = e.what();
        }
        if (!status.empty()) {
            why = status;
            return false;
        }
        return true;
    }

    struct Request {
        Device* self;
        std::string* status;
        Done done;
    };

    void requestDevice(std::string& status, const Done& done) {
        if (!openswmm_webgpu_present()) {
            status = "this browser has no WebGPU (navigator.gpu)";
            done();
            return;
        }
        instance_ = wgpuCreateInstance(nullptr);
        if (!instance_) {
            status = "WebGPU instance creation failed";
            done();
            return;
        }
        WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
        options.powerPreference = WGPUPowerPreference_HighPerformance;
        WGPURequestAdapterCallbackInfo info = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
        info.mode = WGPUCallbackMode_AllowSpontaneous;
        info.callback = [](WGPURequestAdapterStatus st, WGPUAdapter adapter, WGPUStringView msg,
                           void* u1, void*) {
            auto* r = static_cast<Request*>(u1);
            if (st != WGPURequestAdapterStatus_Success || !adapter) {
                *r->status = "no WebGPU adapter (" + text(msg) + ")";
                r->done();
                delete r;
                return;
            }
            r->self->onAdapter(adapter, r);
        };
        info.userdata1 = new Request{this, &status, done};
        wgpuInstanceRequestAdapter(instance_, &options, info);
    }

    void onAdapter(WGPUAdapter adapter, Request* r) {
        WGPULimits supported = WGPU_LIMITS_INIT;
        wgpuAdapterGetLimits(adapter, &supported);
        // Large meshes need the adapter's full buffer sizes, not the WebGPU defaults.
        WGPULimits required = WGPU_LIMITS_INIT;
        required.maxStorageBufferBindingSize = supported.maxStorageBufferBindingSize;
        required.maxBufferSize = supported.maxBufferSize;
        WGPUDeviceDescriptor desc = WGPU_DEVICE_DESCRIPTOR_INIT;
        desc.label = view("openswmm 2D");
        desc.requiredLimits = &required;
        desc.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*, WGPUErrorType,
                                                       WGPUStringView msg, void* u1, void*) {
            static_cast<Device*>(u1)->fail(text(msg));
        };
        desc.uncapturedErrorCallbackInfo.userdata1 = this;
        desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
        desc.deviceLostCallbackInfo.callback = [](WGPUDevice const*, WGPUDeviceLostReason reason,
                                                  WGPUStringView msg, void* u1, void*) {
            if (reason != WGPUDeviceLostReason_Destroyed)
                static_cast<Device*>(u1)->fail("device lost: " + text(msg));
        };
        desc.deviceLostCallbackInfo.userdata1 = this;
        WGPURequestDeviceCallbackInfo info = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
        info.mode = WGPUCallbackMode_AllowSpontaneous;
        info.callback = [](WGPURequestDeviceStatus st, WGPUDevice device, WGPUStringView msg,
                           void* u1, void*) {
            auto* r = static_cast<Request*>(u1);
            if (st != WGPURequestDeviceStatus_Success || !device) {
                *r->status = "no WebGPU device (" + text(msg) + ")";
                r->done();
                delete r;
                return;
            }
            r->self->onDevice(device, r);
        };
        info.userdata1 = r;
        wgpuAdapterRequestDevice(adapter, &desc, info);
    }

    void onDevice(WGPUDevice d, Request* r) {
        device = d;
        queue = wgpuDeviceGetQueue(device);
        wgpuDeviceGetLimits(device, &limits);
        wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
        WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
        source.code = view(kSurfaceWgsl);
        WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        md.nextInChain = &source.chain;
        WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &md);
        // One explicit layout for every kernel ("auto" layouts drop the bindings an entry point
        // does not use, and one bind group could then not serve them all).
        WGPUBindGroupLayoutEntry entries[9];
        for (std::uint32_t b = 0; b < 9; ++b) {
            entries[b] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            entries[b].binding = b;
            entries[b].visibility = WGPUShaderStage_Compute;
            entries[b].buffer.type = b == 0               ? WGPUBufferBindingType_Uniform
                                     : (b >= 2 && b <= 4) ? WGPUBufferBindingType_ReadOnlyStorage
                                                          : WGPUBufferBindingType_Storage;
        }
        WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        ld.entryCount = 9;
        ld.entries = entries;
        layout = wgpuDeviceCreateBindGroupLayout(device, &ld);
        // Group 1: one uniform holding the base-substep index, selected per dispatch by a dynamic
        // offset; the values 0..2^(kMaxTiers-1)-1 are written once.
        WGPUBindGroupLayoutEntry sub_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        sub_entry.binding = 0;
        sub_entry.visibility = WGPUShaderStage_Compute;
        sub_entry.buffer.type = WGPUBufferBindingType_Uniform;
        sub_entry.buffer.hasDynamicOffset = 1;
        sub_entry.buffer.minBindingSize = 16;
        WGPUBindGroupLayoutDescriptor sld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        sld.entryCount = 1;
        sld.entries = &sub_entry;
        WGPUBindGroupLayout sub_layout = wgpuDeviceCreateBindGroupLayout(device, &sld);
        sub_stride = std::max<std::uint32_t>(16, limits.minUniformBufferOffsetAlignment);
        std::vector<std::uint32_t> values(kSelectors * std::size_t(kSubsteps) * sub_stride / 4, 0u);
        for (std::uint32_t c = 0; c < kSelectors; ++c)
            for (std::uint32_t k = 0; k < kSubsteps; ++k) {
                values[subOffset(k, c) / 4] = k;
                values[subOffset(k, c) / 4 + 1] = c;
            }
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.label = view("substeps");
        bd.size = values.size() * 4;
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        WGPUBuffer sub_buffer = wgpuDeviceCreateBuffer(device, &bd);
        wgpuQueueWriteBuffer(queue, sub_buffer, 0, values.data(), values.size() * 4);
        WGPUBindGroupEntry sub_bind = WGPU_BIND_GROUP_ENTRY_INIT;
        sub_bind.binding = 0;
        sub_bind.buffer = sub_buffer;
        sub_bind.size = 16;
        WGPUBindGroupDescriptor sgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        sgd.layout = sub_layout;
        sgd.entryCount = 1;
        sgd.entries = &sub_bind;
        sub_group = wgpuDeviceCreateBindGroup(device, &sgd);
        WGPUBindGroupLayout layouts[2] = {layout, sub_layout};
        WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pd.bindGroupLayoutCount = 2;
        pd.bindGroupLayouts = layouts;
        WGPUPipelineLayout pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pd);
        for (int k = 0; k < kPipeCount; ++k) {
            WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
            cd.layout = pipeline_layout;
            cd.compute.module = module;
            cd.compute.entryPoint = view(kEntry[k]);
            pipe[k] = wgpuDeviceCreateComputePipeline(device, &cd);
        }
        wgpuPipelineLayoutRelease(pipeline_layout);
        wgpuBindGroupLayoutRelease(sub_layout);
        wgpuBufferRelease(sub_buffer); // the bind group keeps it alive
        wgpuShaderModuleRelease(module);
        WGPUPopErrorScopeCallbackInfo info = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
        info.mode = WGPUCallbackMode_AllowSpontaneous;
        info.callback = [](WGPUPopErrorScopeStatus, WGPUErrorType type, WGPUStringView msg,
                           void* u1, void*) {
            auto* r = static_cast<Request*>(u1);
            if (type != WGPUErrorType_NoError)
                *r->status = "WebGPU kernels failed to compile: " + text(msg);
            r->done();
            delete r;
        };
        info.userdata1 = r;
        wgpuDevicePopErrorScope(device, info);
    }

    pthread_t thread_{};
    WGPUInstance instance_ = nullptr;
    std::string error_;
};

/// Per-solver device buffers (created and released on the GPU thread).
struct Buffers {
    WGPUBuffer params = nullptr, ctl = nullptr, cell_g = nullptr, face_g = nullptr, idx = nullptr,
               cell_f = nullptr, cell_u = nullptr, face_s = nullptr, face_u = nullptr,
               pub_stage = nullptr;
    WGPUBindGroup group = nullptr;
    std::uint64_t pub_bytes = 0;
};

namespace {

WGPUBuffer makeBuffer(WGPUDevice device, std::uint64_t bytes, WGPUBufferUsage usage,
                      const char* label) {
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.label = view(label);
    d.size = std::max<std::uint64_t>(16, (bytes + 3) / 4 * 4); // never an empty binding
    d.usage = usage;
    return wgpuDeviceCreateBuffer(device, &d);
}

void release(WGPUBuffer& b) {
    if (b) {
        wgpuBufferDestroy(b);
        wgpuBufferRelease(b);
        b = nullptr;
    }
}

std::uint32_t groups(std::uint32_t threads, std::uint32_t size) {
    return std::max<std::uint32_t>(1, (threads + size - 1) / size);
}

/// Map @p bytes of @p stage for reading, hand them to @p use, unmap, then call @p done.
struct MapJob {
    Device* device;
    WGPUBuffer stage;
    std::size_t bytes;
    std::function<void(const void*)> use;
    Done done;
};
void mapThen(Device& device, WGPUBuffer stage, std::size_t bytes,
             std::function<void(const void*)> use, const Done& done) {
    WGPUBufferMapCallbackInfo info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = [](WGPUMapAsyncStatus st, WGPUStringView msg, void* u1, void*) {
        auto* job = static_cast<MapJob*>(u1);
        if (st == WGPUMapAsyncStatus_Success) {
            job->use(wgpuBufferGetConstMappedRange(job->stage, 0, job->bytes));
            wgpuBufferUnmap(job->stage);
        } else {
            job->device->fail("reading results back failed: " + text(msg));
        }
        job->done();
        delete job;
    };
    info.userdata1 = new MapJob{&device, stage, bytes, std::move(use), done};
    wgpuBufferMapAsync(stage, WGPUMapMode_Read, 0, bytes, info);
}

/// Submit @p encoder's commands.
void submit(Device& device, WGPUCommandEncoder encoder) {
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(device.queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
}

} // namespace

// ===========================================================================
// Factory helpers
// ===========================================================================

std::string unsupportedOptions(const SolverOptions2D& opts) {
    std::string out;
    auto add = [&](const char* what) {
        if (!out.empty())
            out += ", ";
        out += what;
    };
    return out;
}

std::unique_ptr<ISurfaceSolver> makeWebGpuSurfaceSolver(std::string& why) {
    Device* device = Device::get(why);
    if (!device)
        return nullptr;
    return std::make_unique<WebGpuSurfaceSolver>(*device);
}

// ===========================================================================
// WebGpuSurfaceSolver
// ===========================================================================

WebGpuSurfaceSolver::WebGpuSurfaceSolver(Device& device) : dev_(device) {}

WebGpuSurfaceSolver::~WebGpuSurfaceSolver() {
    try {
        finalize();
    } catch (...) {
        // A lost device cannot release anything; the buffers die with it.
    }
}

void WebGpuSurfaceSolver::initialize(MeshData& mesh, SurfaceStateData& state,
                                     SolverOptions2D& opts) {
    // Model state the factory cannot see. Refused loudly, never substituted (R6).
    {
        std::string lacking = unsupportedOptions(opts);
        auto add = [&](const char* what) {
            if (!lacking.empty())
                lacking += ", ";
            lacking += what;
        };
        if (state.heat.enabled)
            add("surface heat exchange");
        if (state.live_passive_exchange)
            add("live passive exchange");
        if (!lacking.empty())
            throw std::invalid_argument("BACKEND WEBGPU does not implement " + lacking +
                                        " yet; use BACKEND CPU for this model.");
    }
    mesh_ = &mesh;
    state_ = &state;
    opts_ = &opts;
    swe_ = opts.momentum == Momentum2D::FULL_SWE;
    diffusive_ = opts.momentum == Momentum2D::DIFFUSIVE_WAVE;
    // FRONT_REBUILD AUTO is on for FULL_SWE, where it is part of the scheme (== the other marchers).
    front_ = opts.front_rebuild < 0 ? swe_ : opts.front_rebuild != 0;
    if (swe_ && opts.cfl_number > 0.5) {
        // Positivity bound of the first-order Godunov update (== the other marchers).
        if (opts.cfl_number != SolverOptions2D{}.cfl_number)
            std::fprintf(stderr,
                         "[openswmm 2D] MOMENTUM_EQUATION FULL_SWE: CFL_NUMBER %.3g "
                         "reduced to 0.5 (positivity bound of the Godunov update).\n",
                         opts.cfl_number);
        opts.cfl_number = 0.5;
    }
    order2_ = swe_ && opts.reconstruction_order == 2;
    // Momentum physics (== the other marchers' initialize): the WALL_DRAG edges set
    // wall_drag_enabled, then the scheme and the model must allow it.
    std::vector<int> wd_slot;
    std::vector<double> wd_cw;
    if (state.boundary)
        for (const auto& [stored_slot, cw] : state.boundary->edge_wall_drag.entries()) {
            const int slot = int(stored_slot);
            if (!std::isfinite(cw) || cw < 0)
                throw std::invalid_argument("WALL_DRAG requires finite nonnegative Cw.");
            if (cw == 0)
                continue;
            const int i = MeshData::slot_cell(slot), e = MeshData::slot_local(slot);
            if (i >= mesh.n_cells() || e >= mesh.cell_vertex_count(i) ||
                mesh.cell_neighbour(i, e) >= 0 ||
                state.boundary->edge_bc_type[slot] != int8_t(BoundaryType::WALL))
                throw std::invalid_argument("WALL_DRAG must name an exterior WALL edge.");
            wd_slot.push_back(slot);
            wd_cw.push_back(cw);
        }
    opts.wall_drag_enabled = !wd_slot.empty();
    if (!std::isfinite(opts.eddy_viscosity_background) || opts.eddy_viscosity_background < 0)
        throw std::invalid_argument("EDDY_VISCOSITY_BACKGROUND must be finite and nonnegative.");
    physics_ = opts.momentumPhysics();
    if (physics_) {
        if (!(opts.momentum == Momentum2D::FULL_SWE) || opts.lts_tiers != 1)
            throw std::invalid_argument(
                "Coriolis/Smagorinsky/wall drag require FULL_SWE and LTS_TIERS 1.");
        if (opts.coriolis == Coriolis2D::LATITUDE && !opts.coriolis_latitude_set)
            throw std::invalid_argument("CORIOLIS LATITUDE requires CORIOLIS_LATITUDE.");
        if (opts.coriolis == Coriolis2D::CONSTANT_F && !opts.coriolis_f_set)
            throw std::invalid_argument("CORIOLIS CONSTANT_F requires CORIOLIS_F.");
        if (!std::isfinite(opts.coriolisParameter()) || !std::isfinite(opts.smagorinsky_cs) ||
            !std::isfinite(opts.mixing_length_coefficient) || opts.mixing_length_coefficient < 0 ||
            opts.smagorinsky_cs < 0 || !std::isfinite(opts.coriolis_latitude) ||
            std::abs(opts.coriolis_latitude) > 90)
            throw std::invalid_argument("Invalid Coriolis/Smagorinsky parameters.");
        if (state.transport.active() || state.heat.enabled || state.snow.active() ||
            (state.node_coupling && !state.node_coupling->empty()))
            throw std::invalid_argument("Coriolis/Smagorinsky/wall drag currently require "
                                        "uncoupled surface hydraulics.");
        std::fprintf(stderr,
                     "[openswmm 2D] optional momentum physics: f=%.17g s^-1, turbulence Cs=%.17g, "
                     "mixing-length c=%.17g, background nu=%.17g m2/s; WebGPU host-driven global "
                     "stepping, f32 mixing acceptance tolerance %.0e.\n",
                     opts.coriolisParameter(), opts.smagorinsky_cs, opts.mixing_length_coefficient,
                     opts.eddy_viscosity_background, kMixingEnergyTolerance);
    }
    if (order2_) {
        // Second order runs in global steps (== the other marchers).
        if (opts.lts_tiers > 1) {
            std::fprintf(stderr,
                         "[openswmm 2D] RECONSTRUCTION_ORDER 2 runs in global-dt mode: "
                         "LTS_TIERS %d reduced to 1.\n",
                         opts.lts_tiers);
            opts.lts_tiers = 1;
        }
        if (mesh.vert_stencil_ptr.size() != std::size_t(mesh.n_vertices() + 1))
            throw std::invalid_argument(
                "BACKEND WEBGPU: RECONSTRUCTION_ORDER 2 requires built vertex stencils");
    }
    nc_ = mesh.n_triangles();
    state.boundary_receipt_slots.clear();
    state.boundary_in_applied.clear();
    state.boundary_out_applied.clear();
    state.boundary_requested.clear();
    state.boundary_last_flow.clear();
    state.boundary_last_dt.clear();
    state.boundary_requested_mass.clear();
    state.boundary_applied_mass.clear();
    if (nc_ <= 0)
        return;

    edges_.build(mesh);
    ne_ = edges_.ne;
    nslots_ = static_cast<int>(state.edge_flux.size());
    perot_ = !swe_ && (opts.theta < 1.0 || opts.advection);
    const std::size_t nc = static_cast<std::size_t>(nc_), ne = static_cast<std::size_t>(ne_);

    // Non-WALL boundary edges, cell by cell (the Kokkos scan): each cell's edges form one run,
    // whose cell is kept active (pinned) like the other marchers' boundary cells.
    bc_cell_.clear();
    bc_slot_.clear();
    std::vector<std::uint32_t> run_ptr;
    if (state.boundary)
        for (int i = 0; i < nc_; ++i)
            for (int e = 0; e < mesh.cell_vertex_count(i); ++e) {
                const int slot = MeshData::slot(i, e);
                if (mesh.cell_neighbour(i, e) >= 0 ||
                    static_cast<BoundaryType>(state.boundary->edge_bc_type[slot]) ==
                        BoundaryType::WALL)
                    continue;
                if (bc_cell_.empty() || bc_cell_.back() != i)
                    run_ptr.push_back(static_cast<std::uint32_t>(bc_cell_.size()));
                bc_cell_.push_back(i);
                bc_slot_.push_back(slot);
            }
    nbc_ = static_cast<int>(bc_cell_.size());
    nruns_ = static_cast<int>(run_ptr.size());
    run_ptr.push_back(static_cast<std::uint32_t>(nbc_));
    const std::size_t nbc = static_cast<std::size_t>(nbc_);
    state.boundary_receipt_slots = bc_slot_;
    state.boundary_in_applied.assign(nbc, 0.0);
    state.boundary_out_applied.assign(nbc, 0.0);
    state.boundary_requested.assign(nbc, 0.0);
    state.boundary_last_flow.assign(nbc, 0.0);
    state.boundary_last_dt.assign(nbc, 0.0);
    bc_type_.assign(nbc, 0u);
    bc_slope_.assign(nbc, 0.0f);
    bc_drive_.assign(3 * nbc, 0.0f);
    pub_bc_s_.assign(kBcSFields * nbc, 0.0f);
    pub_bc_u_.assign(kBcUFields * nbc, 0u);

    // Transport rows (== the Kokkos initialize): [2D_BOUNDARY_QUALITY] rows onto this solver's
    // boundary entries, with the serial marcher's refusals.
    ns_ = state.transport.active() ? state.transport.n_species : 0;
    const std::size_t ns = static_cast<std::size_t>(ns_);
    state.boundary_requested_mass.assign(nbc * ns, 0.0);
    state.boundary_applied_mass.assign(nbc * ns, 0.0);
    if (ns_ > 0) {
        auto& tr = state.transport;
        if (tr.cell_mass.size() != ns * nc)
            throw std::invalid_argument("BACKEND WEBGPU: transport rows are not sized to the mesh");
        tr.bc_conc.assign(nbc * ns, 0.0);
        for (const auto& row : tr.bc_quality_rows) {
            const auto it = std::find(bc_slot_.begin(), bc_slot_.end(), row.slot);
            if (it == bc_slot_.end())
                throw std::runtime_error("[2D_BOUNDARY_QUALITY] edge slot " +
                                         std::to_string(row.slot) +
                                         " is not a non-WALL boundary edge, so its "
                                         "concentration could never enter the mesh.");
            if (row.species < 0 || static_cast<std::size_t>(row.species) >= ns)
                throw std::runtime_error("[2D_BOUNDARY_QUALITY] species index out of range.");
            tr.bc_conc[std::size_t(it - bc_slot_.begin()) * ns + std::size_t(row.species)] =
                row.conc;
        }
    }

    // f32 data on a local origin and a shifted elevation datum: projected coordinates and
    // absolute elevations would otherwise cost most of the f32 mantissa.
    x0_ = *std::min_element(mesh.vx.begin(), mesh.vx.end());
    y0_ = *std::min_element(mesh.vy.begin(), mesh.vy.end());
    z0_ = *std::min_element(mesh.tri_cz.begin(), mesh.tri_cz.begin() + nc_);

    const bool vfr = opts.cell_closure == CellClosure2D::VFR;
    const bool vfr_face = opts.face_reconstruction == FaceDepth2D::VFR_FACE;
    const bool quads = mesh.n_quads() != 0;
    // The closure block also carries the Manning n that DIFFUSIVE_WAVE and FULL_SWE read.
    const std::size_t cell_fields =
        (vfr || quads || swe_ || diffusive_) ? kCellGClosureFields : kCellGFields;
    std::vector<float> cell_g(cell_fields * nc, 0.0f);
    // Each cell's reference bed (FP64): the device keeps its head relative to it. FLAT: the mean
    // bed; VFR: the lowest bed, the origin of the local closure.
    std::vector<double>& zref = zref_;
    zref.assign(nc, 0.0);
    vfr_ = vfr;
    for (std::size_t i = 0; i < nc; ++i) {
        zref[i] = mesh.tri_cz[i];
        if (!vfr)
            continue;
        if (mesh.cell_vertex_count(static_cast<int>(i)) == 4) {
            zref[i] = std::min(mesh.quad_vfr_z[i * kQuadVfrZ], mesh.quad_vfr_z[i * kQuadVfrZ + 3]);
        } else {
            zref[i] = mesh.vz[mesh.cell_v[i * kMaxCellVerts]];
            for (int k = 1; k < 3; ++k)
                zref[i] = std::min(zref[i], mesh.vz[mesh.cell_v[i * kMaxCellVerts + k]]);
        }
    }
    for (std::size_t i = 0; i < nc; ++i) {
        cell_g[kArea * nc + i] = static_cast<float>(mesh.tri_area[i]);
        cell_g[kZref * nc + i] = static_cast<float>(zref[i] - z0_);
        cell_g[kCx * nc + i] = static_cast<float>(mesh.tri_cx[i] - x0_);
        cell_g[kCy * nc + i] = static_cast<float>(mesh.tri_cy[i] - y0_);
        // FULL_SWE bounds its step by the positivity length instead (== the other marchers).
        cell_g[kLchar * nc + i] =
            static_cast<float>(swe_ ? edges_.cell_lpos[i] : edges_.cell_lchar[i]);
        if (cell_fields == kCellGFields)
            continue;
        cell_g[kMann * nc + i] = static_cast<float>(mesh.mannings_n[i]);
        // Closure data in the cell's local frame (relative to its lowest bed, differences in
        // FP64): a triangle's sorted vertex beds, or a quad's two sorted sub-triangle triples
        // (MeshData::quad_vfr_z) and their areas.
        const int nv = mesh.cell_vertex_count(static_cast<int>(i));
        cell_g[kNv * nc + i] = static_cast<float>(nv);
        if (nv == 4) {
            const double* zs = &mesh.quad_vfr_z[i * kQuadVfrZ];
            const double zlow = std::min(zs[0], zs[3]);
            for (int k = 0; k < kQuadVfrZ; ++k)
                cell_g[(kZ1 + k) * nc + i] = static_cast<float>(zs[k] - zlow);
            cell_g[kQuadA1 * nc + i] = static_cast<float>(mesh.quad_vfr_a[2 * i]);
            cell_g[kQuadA2 * nc + i] = static_cast<float>(mesh.quad_vfr_a[2 * i + 1]);
        } else {
            double z1 = mesh.vz[mesh.cell_v[i * kMaxCellVerts + 0]];
            double z2 = mesh.vz[mesh.cell_v[i * kMaxCellVerts + 1]];
            double z3 = mesh.vz[mesh.cell_v[i * kMaxCellVerts + 2]];
            vfrSort3(z1, z2, z3);
            cell_g[kZ1 * nc + i] = 0.0f;
            cell_g[kZ2 * nc + i] = static_cast<float>(z2 - z1);
            cell_g[kZ3 * nc + i] = static_cast<float>(z3 - z1);
        }
    }
    const std::size_t face_fields = vfr_face ? kFaceGVfrFields : kFaceGFields;
    std::vector<float> face_g(face_fields * ne, 0.0f);
    for (std::size_t e = 0; e < ne; ++e) {
        face_g[kXi * ne + e] = static_cast<float>(edges_.xi[e]);
        // Offsets to the face's crest bed and between the two reference beds, in FP64.
        const double crest = vfr_face ? edges_.ze_lo[e] : edges_.zface[e];
        const std::size_t a = static_cast<std::size_t>(edges_.cL[e]);
        const std::size_t c = static_cast<std::size_t>(edges_.cR[e]);
        face_g[kOffL * ne + e] = static_cast<float>(zref[a] - crest);
        face_g[kOffR * ne + e] = static_cast<float>(zref[c] - crest);
        face_g[kDref * ne + e] = static_cast<float>(zref[c] - zref[a]);
        face_g[kInvDx * ne + e] = static_cast<float>(edges_.inv_dx_normal[e]);
        // The face's n^2 for the inertial law, its n for the diffusive one.
        face_g[kN2 * ne + e] =
            static_cast<float>(diffusive_ ? edges_.n_face[e] : edges_.n2_face[e]);
        face_g[kNx * ne + e] = static_cast<float>(edges_.nx[e]);
        face_g[kNy * ne + e] = static_cast<float>(edges_.ny[e]);
        face_g[kMx * ne + e] = static_cast<float>(edges_.mx[e] - x0_);
        face_g[kMy * ne + e] = static_cast<float>(edges_.my[e] - y0_);
        face_g[kConv * ne + e] = static_cast<float>(edges_.conveyance[e]);
        if (vfr_face)
            face_g[kDz * ne + e] = static_cast<float>(edges_.ze_hi[e] - edges_.ze_lo[e]);
    }
    const std::uint32_t off_bc_g = static_cast<std::uint32_t>(face_g.size());
    face_g.resize(face_g.size() + kBcGFields * nbc, 0.0f);
    for (std::size_t k = 0; k < nbc; ++k) {
        const int i = bc_cell_[k], slot = bc_slot_[k];
        int va = 0, vb = 0;
        mesh.cell_edge_vertices(i, slot % kMaxCellVerts, va, vb);
        face_g[off_bc_g + kBcLength * nbc + k] = static_cast<float>(mesh.edge_length[slot]);
        face_g[off_bc_g + kBcN * nbc + k] = static_cast<float>(mesh.mannings_n[i]);
        face_g[off_bc_g + kBcMx * nbc + k] =
            static_cast<float>(0.5 * (mesh.vx[va] + mesh.vx[vb]) - x0_);
        face_g[off_bc_g + kBcMy * nbc + k] =
            static_cast<float>(0.5 * (mesh.vy[va] + mesh.vy[vb]) - y0_);
        // The edge's crest bed: the cell's mean bed under MEAN faces (inertial::faceFlowDepth
        // against tri_cz), the lower endpoint bed under VFR_FACE.
        const double zlo = std::min(mesh.vz[va], mesh.vz[vb]);
        const double crest = vfr_face ? zlo : mesh.tri_cz[i];
        face_g[off_bc_g + kBcOff * nbc + k] = static_cast<float>(zref[std::size_t(i)] - crest);
        face_g[off_bc_g + kBcDz * nbc + k] =
            static_cast<float>(std::max(mesh.vz[va], mesh.vz[vb]) - zlo);
        // The ghost's distance across the edge: 2A/(3L) for a triangle, the centroid-to-edge
        // normal distance for a quad (== the CPU stage boundary).
        const double L = mesh.edge_length[slot];
        const double dist = mesh.cell_vertex_count(i) == 3 ? 2.0 * mesh.tri_area[i] / (3.0 * L)
                                                           : mesh.edge_dist_c(slot);
        face_g[off_bc_g + kBcInvDist * nbc + k] =
            (L > 1.0e-12 && dist > 0.0) ? static_cast<float>(1.0 / dist) : 0.0f;
        face_g[off_bc_g + kBcNx * nbc + k] = static_cast<float>(mesh.edge_nx[slot]);
        face_g[off_bc_g + kBcNy * nbc + k] = static_cast<float>(mesh.edge_ny[slot]);
    }
    // FULL_SWE: each cell's sum of n L over its boundary entries (the cell update books their
    // share of its hydrostatic pressure, as the FP64 marchers' sum leaves it before their
    // boundary pass).
    if (swe_)
        for (std::size_t k = 0; k < nbc; ++k) {
            const auto i = static_cast<std::size_t>(bc_cell_[k]);
            const auto slot = static_cast<std::size_t>(bc_slot_[k]);
            cell_g[kSbcX * nc + i] +=
                static_cast<float>(mesh.edge_nx[slot] * mesh.edge_length[slot]);
            cell_g[kSbcY * nc + i] +=
                static_cast<float>(mesh.edge_ny[slot] * mesh.edge_length[slot]);
        }
    // FULL_SWE: each cell's exterior WALL edges, in ascending slot order (== the other marchers).
    std::vector<std::uint32_t> wall_ptr(1, 0);
    std::vector<float> wall_g;
    if (swe_) {
        std::vector<int> walls;
        for (int i = 0; i < nc_; ++i) {
            for (int e = 0; e < mesh.cell_vertex_count(i); ++e) {
                const int slot = MeshData::slot(i, e);
                if (mesh.cell_neighbour(i, e) >= 0 ||
                    (state.boundary &&
                     static_cast<BoundaryType>(state.boundary->edge_bc_type[slot]) !=
                         BoundaryType::WALL))
                    continue;
                walls.push_back(slot);
            }
            wall_ptr.push_back(static_cast<std::uint32_t>(walls.size()));
        }
        const std::size_t nw = walls.size();
        wall_g.assign(kWallGFields * nw, 0.0f);
        for (std::size_t w = 0; w < nw; ++w) {
            const int slot = walls[w];
            int va = 0, vb = 0;
            mesh.cell_edge_vertices(slot / kMaxCellVerts, slot % kMaxCellVerts, va, vb);
            wall_g[kWallNx * nw + w] = static_cast<float>(mesh.edge_nx[slot]);
            wall_g[kWallNy * nw + w] = static_cast<float>(mesh.edge_ny[slot]);
            wall_g[kWallLen * nw + w] = static_cast<float>(mesh.edge_length[slot]);
            wall_g[kWallMx * nw + w] = static_cast<float>(0.5 * (mesh.vx[va] + mesh.vx[vb]) - x0_);
            wall_g[kWallMy * nw + w] = static_cast<float>(0.5 * (mesh.vy[va] + mesh.vy[vb]) - y0_);
        }
    } else {
        wall_ptr.assign(std::size_t(nc) + 1, 0u);
    }
    const std::uint32_t nw = wall_ptr.back();
    const std::uint32_t off_wall_g = static_cast<std::uint32_t>(face_g.size());
    face_g.insert(face_g.end(), wall_g.begin(), wall_g.end());
    const std::uint32_t off_ptr = static_cast<std::uint32_t>(4 * ne);
    const std::uint32_t off_edge = off_ptr + static_cast<std::uint32_t>(nc + 1);
    std::vector<std::uint32_t> idx(off_edge + edges_.cell_edge.size());
    for (std::size_t e = 0; e < ne; ++e) {
        idx[e] = static_cast<std::uint32_t>(edges_.cL[e]);
        idx[ne + e] = static_cast<std::uint32_t>(edges_.cR[e]);
        idx[2 * ne + e] = static_cast<std::uint32_t>(edges_.slotL[e]);
        idx[3 * ne + e] = static_cast<std::uint32_t>(edges_.slotR[e]);
    }
    for (std::size_t i = 0; i <= nc; ++i)
        idx[off_ptr + i] = static_cast<std::uint32_t>(edges_.cell_ptr[i]);
    for (std::size_t p = 0; p < edges_.cell_edge.size(); ++p)
        idx[off_edge + p] = static_cast<std::uint32_t>(edges_.cell_edge[p]) |
                            (edges_.cell_sign[p] < 0 ? 0x80000000u : 0u);
    const std::uint32_t off_bc_idx = static_cast<std::uint32_t>(idx.size());
    for (const int c : bc_cell_)
        idx.push_back(static_cast<std::uint32_t>(c));
    idx.insert(idx.end(), run_ptr.begin(), run_ptr.end());
    idx.resize(idx.size() + nbc, 0u); // types, written per advance

    // Junction exchange points (non-outfall; owned by the router, frozen topology). Each point
    // works on the datum zp of its coupling cell's reference bed; every offset from it is taken
    // in FP64 here, so the device forms head differences from small numbers.
    const std::vector<CouplingPoint>* points =
        (state.node_coupling && state.nodes_1d) ? state.node_coupling : nullptr;
    ncp_ = points ? static_cast<int>(points->size()) : 0;
    const std::size_t np = static_cast<std::size_t>(ncp_);
    cp_node_.assign(np, -1);
    cp_zp_.assign(np, 0.0);
    node_slot_node_.clear();
    std::vector<std::uint32_t> cp_i(kCpIFields * np, 0u), st_cell;
    std::vector<float> cp_g(kCpGFields * np, 0.0f), st_w, st_o;
    std::vector<int> group_ptr, group_item;
    if (np > 0) {
        const auto& pts = *points;
        const std::vector<int>* head_cells = state.coupling_head_cells;
        std::map<int, int> slot_of_node;
        for (std::size_t k = 0; k < np; ++k) {
            const CouplingPoint& cp = pts[k];
            const int ci = cp.cell_idx, vtx = cp.vertex_idx;
            const auto [it, fresh] =
                slot_of_node.try_emplace(cp.node_idx, static_cast<int>(slot_of_node.size()));
            if (fresh)
                node_slot_node_.push_back(cp.node_idx);
            cp_node_[k] = cp.node_idx;
            const double zp = ci >= 0 ? zref[static_cast<std::size_t>(ci)] : 0.0;
            cp_zp_[k] = zp;
            auto ci_at = [&](CpI f) -> std::uint32_t& { return cp_i[f * np + k]; };
            auto cg_at = [&](CpG f) -> float& { return cp_g[f * np + k]; };
            ci_at(kCpCell) = ci >= 0 ? static_cast<std::uint32_t>(ci) : kCpNoCell;
            ci_at(kCpNode) = static_cast<std::uint32_t>(it->second);
            ci_at(kCpLaw) = static_cast<std::uint32_t>(cp.law);
            ci_at(kCpFlags) = cp.equalize ? kCpFlagEqualize : 0u;
            // The head stencil (couplingHead2D's order of precedence).
            ci_at(kCpHb) = static_cast<std::uint32_t>(st_cell.size());
            auto entry = [&](int t, double w, double offset) {
                st_cell.push_back(static_cast<std::uint32_t>(t));
                st_w.push_back(static_cast<float>(w));
                st_o.push_back(static_cast<float>(offset));
            };
            if (ci < 0) {
                ci_at(kCpMode) = 0;
            } else if (cp.head_end > cp.head_begin && head_cells) {
                ci_at(kCpMode) = 2; // COUPLING_HEAD VERTEX / STENCIL_WET_MEAN; else the cell
                ci_at(kCpFlags) |= kCpFlagFallbackCell;
                for (int p = cp.head_begin; p < cp.head_end; ++p) {
                    const int t = (*head_cells)[static_cast<std::size_t>(p)];
                    entry(t, 0.0, zref[static_cast<std::size_t>(t)] - zp);
                }
            } else if (vtx < 0) {
                ci_at(kCpMode) = 0;
            } else if (vfr) {
                ci_at(kCpMode) = 2; // wet-masked vertex head; else the vertex bed
                cg_at(kCpFall) = static_cast<float>(mesh.vz[vtx] - zp);
                for (int p = mesh.vert_stencil_ptr[vtx]; p < mesh.vert_stencil_ptr[vtx + 1]; ++p) {
                    const int t = mesh.vert_stencil_idx[p];
                    entry(t, 0.0, zref[static_cast<std::size_t>(t)] - zp);
                }
            } else {
                ci_at(kCpMode) = 1; // the pseudo-Laplacian vertex head
                double c = -zp;
                for (int p = mesh.vert_stencil_ptr[vtx]; p < mesh.vert_stencil_ptr[vtx + 1]; ++p) {
                    const int t = mesh.vert_stencil_idx[p];
                    entry(t, mesh.vert_stencil_wt[p], 0.0);
                    c += mesh.vert_stencil_wt[p] * zref[static_cast<std::size_t>(t)];
                }
                cg_at(kCpConst) = static_cast<float>(c);
            }
            ci_at(kCpHe) = static_cast<std::uint32_t>(st_cell.size());
            // The cells whose depth gates a drain: the vertex stencil, else the cell.
            ci_at(kCpDb) = static_cast<std::uint32_t>(st_cell.size());
            if (vtx >= 0) {
                for (int p = mesh.vert_stencil_ptr[vtx]; p < mesh.vert_stencil_ptr[vtx + 1]; ++p)
                    entry(mesh.vert_stencil_idx[p], 0.0, 0.0);
            } else if (ci >= 0) {
                entry(ci, 0.0, 0.0);
            }
            ci_at(kCpDe) = static_cast<std::uint32_t>(st_cell.size());
            cg_at(kCpCd) = static_cast<float>(cp.cd);
            cg_at(kCpArea) = static_cast<float>(cp.area);
            cg_at(kCpZc) = static_cast<float>(cp.geo.z_crest - zp);
            cg_at(kCpLw) = static_cast<float>(cp.geo.l_weir);
            cg_at(kCpAo) = static_cast<float>(cp.geo.a_orif);
            cg_at(kCpCw) = static_cast<float>(cp.coef.c_w);
            cg_at(kCpCsw) = static_cast<float>(cp.coef.c_sw);
            cg_at(kCpCoi) = static_cast<float>(cp.coef.c_oi);
            cg_at(kCpCoo) = static_cast<float>(cp.coef.c_oo);
            cg_at(kCpHeps) = static_cast<float>(cp.coef.h_eps);
            const auto ni = static_cast<std::size_t>(cp.node_idx);
            const auto& nodes = *state.nodes_1d;
            cg_at(kCpCrown) = static_cast<float>(
                (nodes.invert_elev[ni] + nodes.full_depth[ni]) * opts.len_1d_to_2d - zp);
        }
        // Points interact only through a cell one of them drains or fills (which another may
        // read through a stencil) or a shared node's spill budget; points linked that way form
        // a group that fires in point order on one thread (== the Kokkos grouping).
        std::vector<int> root(np);
        std::iota(root.begin(), root.end(), 0);
        auto find = [&](int a) {
            while (root[a] != a)
                a = root[a] = root[root[a]];
            return a;
        };
        auto join = [&](int a, int b) {
            a = find(a);
            b = find(b);
            if (a != b)
                root[std::max(a, b)] = std::min(a, b);
        };
        std::vector<int> cell_writer(nc, -1);
        std::map<int, int> node_user;
        for (int k = 0; k < ncp_; ++k) {
            const int ci = pts[k].cell_idx;
            if (ci < 0)
                continue;
            int& writer = cell_writer[static_cast<std::size_t>(ci)];
            if (writer < 0)
                writer = k;
            else
                join(k, writer);
            const auto [it, fresh] = node_user.try_emplace(pts[k].node_idx, k);
            if (!fresh)
                join(k, it->second);
        }
        for (int k = 0; k < ncp_; ++k) {
            if (pts[k].cell_idx < 0)
                continue;
            const auto b = cp_i[kCpHb * np + k], e = cp_i[kCpDe * np + k];
            for (std::uint32_t p = b; p < e; ++p) // the head and availability stencils
                if (cell_writer[st_cell[p]] >= 0)
                    join(k, cell_writer[st_cell[p]]);
        }
        group_item.resize(np);
        std::iota(group_item.begin(), group_item.end(), 0);
        std::stable_sort(group_item.begin(), group_item.end(),
                         [&](int a, int b) { return find(a) < find(b); });
        for (int p = 0; p < ncp_; ++p)
            if (p == 0 || find(group_item[p]) != find(group_item[p - 1]))
                group_ptr.push_back(p);
        ncp_groups_ = static_cast<int>(group_ptr.size());
        group_ptr.push_back(ncp_);
    } else {
        ncp_groups_ = 0;
    }
    ncp_nodes_ = static_cast<int>(node_slot_node_.size());
    const std::uint32_t off_cp_idx = static_cast<std::uint32_t>(idx.size());
    idx.insert(idx.end(), cp_i.begin(), cp_i.end());
    for (const int p : group_ptr)
        idx.push_back(static_cast<std::uint32_t>(p));
    for (const int p : group_item)
        idx.push_back(static_cast<std::uint32_t>(p));
    idx.insert(idx.end(), st_cell.begin(), st_cell.end());
    idx.resize(idx.size() + 2 * std::size_t(ncp_nodes_), 0u); // spill budgets, per advance
    const std::uint32_t off_wall_ptr = static_cast<std::uint32_t>(idx.size());
    idx.insert(idx.end(), wall_ptr.begin(), wall_ptr.end());

    // RECONSTRUCTION_ORDER 2: vertex stencils, cell vertices, arms and bed offsets (FP64 here).
    const std::uint32_t nv = order2_ ? static_cast<std::uint32_t>(mesh.n_vertices()) : 0u;
    std::uint32_t off_vs_ptr = 0, off_vs_off = 0, off_cell_v = 0, off_cell_arm = 0, off_csr_arm = 0;
    const std::uint32_t ncsr = static_cast<std::uint32_t>(edges_.cell_edge.size());
    if (order2_) {
        off_vs_ptr = static_cast<std::uint32_t>(idx.size());
        for (std::size_t v = 0; v <= nv; ++v)
            idx.push_back(static_cast<std::uint32_t>(mesh.vert_stencil_ptr[v]));
        off_vs_off = static_cast<std::uint32_t>(face_g.size());
        for (std::size_t v = 0; v < nv; ++v)
            for (int p = mesh.vert_stencil_ptr[v]; p < mesh.vert_stencil_ptr[v + 1]; ++p) {
                const auto j = static_cast<std::size_t>(mesh.vert_stencil_idx[p]);
                idx.push_back(static_cast<std::uint32_t>(j));
                face_g.push_back(static_cast<float>(zref[j] - mesh.vz[v]));
            }
        off_cell_v = static_cast<std::uint32_t>(idx.size());
        off_cell_arm = static_cast<std::uint32_t>(face_g.size());
        face_g.resize(face_g.size() + 3 * 4 * nc, 0.0f);
        for (std::size_t i = 0; i < nc; ++i) {
            const int nvc = mesh.cell_vertex_count(static_cast<int>(i));
            for (int k = 0; k < 4; ++k) {
                const int v = k < nvc ? mesh.cell_v[i * kMaxCellVerts + k] : 0;
                idx.push_back(static_cast<std::uint32_t>(v));
                if (k >= nvc)
                    continue;
                face_g[off_cell_arm + (3 * k + 0) * nc + i] =
                    static_cast<float>(mesh.vx[v] - mesh.tri_cx[i]);
                face_g[off_cell_arm + (3 * k + 1) * nc + i] =
                    static_cast<float>(mesh.vy[v] - mesh.tri_cy[i]);
                face_g[off_cell_arm + (3 * k + 2) * nc + i] =
                    static_cast<float>(zref[i] - mesh.vz[v]);
            }
        }
        off_csr_arm = static_cast<std::uint32_t>(face_g.size());
        for (std::size_t p = 0; p < ncsr; ++p)
            face_g.push_back(static_cast<float>(edges_.cell_arm_x[p]));
        for (std::size_t p = 0; p < ncsr; ++p)
            face_g.push_back(static_cast<float>(edges_.cell_arm_y[p]));
    }

    // Momentum physics: the WALL_DRAG edges grouped by cell (slots of one cell are consecutive)
    // and, for the mixing-length model, each centroid's distance to the nearest boundary edge.
    const std::uint32_t nwd = static_cast<std::uint32_t>(wd_slot.size());
    std::uint32_t nwd_groups = 0;
    const std::uint32_t off_wd_idx = static_cast<std::uint32_t>(idx.size());
    const std::uint32_t off_wd_g = static_cast<std::uint32_t>(face_g.size());
    if (physics_) {
        for (const int slot : wd_slot)
            idx.push_back(static_cast<std::uint32_t>(MeshData::slot_cell(slot)));
        for (std::size_t k = 0; k < wd_slot.size(); ++k)
            if (k == 0 || MeshData::slot_cell(wd_slot[k]) != MeshData::slot_cell(wd_slot[k - 1])) {
                idx.push_back(static_cast<std::uint32_t>(k));
                ++nwd_groups;
            }
        idx.push_back(nwd);
        face_g.resize(face_g.size() + 6 * std::size_t(nwd), 0.0f);
        for (std::size_t k = 0; k < wd_slot.size(); ++k) {
            const int slot = wd_slot[k];
            const auto i = static_cast<std::size_t>(MeshData::slot_cell(slot));
            int va = 0, vb = 0;
            mesh.cell_edge_vertices(int(i), MeshData::slot_local(slot), va, vb);
            const double zlo = std::min(mesh.vz[va], mesh.vz[vb]);
            face_g[off_wd_g + 0 * nwd + k] = static_cast<float>(mesh.edge_nx[slot]);
            face_g[off_wd_g + 1 * nwd + k] = static_cast<float>(mesh.edge_ny[slot]);
            face_g[off_wd_g + 2 * nwd + k] = static_cast<float>(mesh.edge_length[slot]);
            face_g[off_wd_g + 3 * nwd + k] = static_cast<float>(wd_cw[k]);
            face_g[off_wd_g + 4 * nwd + k] = static_cast<float>(zref[i] - zlo);
            face_g[off_wd_g + 5 * nwd + k] =
                static_cast<float>(std::max(mesh.vz[va], mesh.vz[vb]) - zlo);
        }
    }
    const std::uint32_t off_walld = static_cast<std::uint32_t>(face_g.size());
    if (physics_) {
        face_g.resize(face_g.size() + nc, 0.0f);
        if (opts.turbulence == Turbulence2D::MIXING_LENGTH && opts.mixing_length_coefficient > 0) {
            std::vector<std::array<double, 4>> segments;
            for (int c = 0; c < mesh.n_cells(); ++c)
                for (int k = 0; k < mesh.cell_vertex_count(c); ++k)
                    if (mesh.cell_neighbour(c, k) < 0) {
                        int a, b;
                        mesh.cell_edge_vertices(c, k, a, b);
                        segments.push_back({mesh.vx[a], mesh.vy[a], mesh.vx[b], mesh.vy[b]});
                    }
            for (std::size_t c = 0; c < nc; ++c) {
                const double px = mesh.tri_cx[c], py = mesh.tri_cy[c];
                double best = std::numeric_limits<double>::infinity();
                for (const auto& g : segments) {
                    const double dx = g[2] - g[0], dy = g[3] - g[1], len2 = dx * dx + dy * dy;
                    const double t =
                        len2 > 0
                            ? std::clamp(((px - g[0]) * dx + (py - g[1]) * dy) / len2, 0.0, 1.0)
                            : 0.0;
                    best = std::min(best, std::hypot(px - g[0] - t * dx, py - g[1] - t * dy));
                }
                face_g[off_walld + c] = static_cast<float>(best);
            }
        }
    }
    const std::uint32_t off_cp_g = static_cast<std::uint32_t>(face_g.size());
    face_g.insert(face_g.end(), cp_g.begin(), cp_g.end());
    face_g.insert(face_g.end(), st_w.begin(), st_w.end());
    face_g.insert(face_g.end(), st_o.begin(), st_o.end());
    cp_drive_.assign(3 * np, 0.0f);
    cp_budget_.assign(2 * std::size_t(ncp_nodes_), 0u);
    pub_cp_u_.assign(2 * np, 0u);
    exch_.assign(np, 0.0);
    exch_span_ = 0.0;

    const double band = std::min(0.001, 0.5 * opts.h_move);
    params_ = Params{};
    params_.g = static_cast<float>(inertial::kGravity);
    params_.dry = static_cast<float>(opts.dry_depth);
    params_.theta = static_cast<float>(opts.theta);
    params_.froude = static_cast<float>(opts.froude_max);
    params_.beta3 = static_cast<float>(opts.exchange_beta / 3.0);
    params_.alpha = static_cast<float>(opts.cfl_number);
    params_.dt_max = static_cast<float>(opts.max_timestep);
    params_.h_on = static_cast<float>(opts.h_move + band);
    params_.h_off = static_cast<float>(std::max(0.0, opts.h_move - band));
    params_.theta_ref = static_cast<float>(opts.theta_reference_step);
    params_.nc = static_cast<std::uint32_t>(nc);
    params_.ne = static_cast<std::uint32_t>(ne);
    params_.nslots = static_cast<std::uint32_t>(nslots_);
    params_.flags = (perot_ ? kFlagPerot : 0u) | (opts.advection ? kFlagAdvection : 0u) |
                    (opts.theta_reference_step > 0.0 ? kFlagTimeBlend : 0u) |
                    (vfr ? kFlagVfr : 0u) | (vfr_face ? kFlagVfrFace : 0u) |
                    (quads ? kFlagQuads : 0u) | (diffusive_ ? kFlagDiffusive : 0u) |
                    (swe_ ? kFlagSwe : 0u) | (front_ ? kFlagFront : 0u) |
                    (order2_ ? kFlagOrder2 : 0u) | (physics_ ? kFlagPhysics : 0u);
    params_.mwf = static_cast<float>(opts.vfr_min_wet_frac);
    params_.rebuild_every = kRebuildEvery;
    params_.tiers = static_cast<std::uint32_t>(std::clamp(opts.lts_tiers, 1, int(kMaxTiers)));
    params_.nsub = 1u << (params_.tiers - 1);
    params_.off_ptr = off_ptr;
    params_.off_edge = off_edge;
    params_.nbc = static_cast<std::uint32_t>(nbc);
    params_.nruns = static_cast<std::uint32_t>(nruns_);
    params_.off_bc_idx = off_bc_idx;
    params_.off_bc_g = off_bc_g;
    // faceS: q | published flux | FULL_SWE momentum accumulators (4 ne) | the boundary tail.
    params_.off_mom = static_cast<std::uint32_t>(2 * ne);
    params_.off_bc_s = static_cast<std::uint32_t>((swe_ ? 6 : 2) * ne);
    params_.off_bc_u = static_cast<std::uint32_t>(kFaceUFields * ne);
    params_.ncp = static_cast<std::uint32_t>(ncp_);
    params_.ncp_groups = static_cast<std::uint32_t>(ncp_groups_);
    params_.ncp_nodes = static_cast<std::uint32_t>(ncp_nodes_);
    params_.nst = static_cast<std::uint32_t>(st_cell.size());
    params_.off_cp_idx = off_cp_idx;
    params_.off_cp_g = off_cp_g;
    params_.off_cp_u = params_.off_bc_u + static_cast<std::uint32_t>(kBcUFields * nbc);
    params_.beta = static_cast<float>(opts.exchange_beta);
    params_.dh_eps = static_cast<float>(opts.flux_dh_eps);
    params_.off_wall_ptr = off_wall_ptr;
    params_.off_wall_g = off_wall_g;
    params_.nw = nw;
    params_.nv = nv;
    params_.off_vs_ptr = off_vs_ptr;
    params_.off_vs_off = off_vs_off;
    params_.off_cell_v = off_cell_v;
    params_.off_cell_arm = off_cell_arm;
    params_.off_csr_arm = off_csr_arm;
    params_.ncsr = ncsr;
    // cellF: the base fields, the order-2 gradients and step momentum, the vertex bounds.
    params_.off_vb = static_cast<std::uint32_t>((order2_ ? kCellFOrder2Fields : kCellFFields) * nc);
    params_.off_ph_f = params_.off_vb + 4 * nv;
    params_.off_ph_u = static_cast<std::uint32_t>(kCellUFields * nc);
    params_.nwd = nwd;
    params_.nwd_groups = nwd_groups;
    params_.off_wd_idx = off_wd_idx;
    params_.off_wd_g = off_wd_g;
    params_.off_walld = off_walld;
    // Transport rows: the faceS regions after the boundary tail (WebGpuKernels.hpp kSp*).
    const std::size_t sp_nodes = ns_ > 0 ? std::size_t(ncp_nodes_) : 0;
    const std::size_t sp_groups = ns_ > 0 ? groups(params_.nc, kWorkgroup) : 0;
    const std::size_t off_sp = std::size_t(params_.off_bc_s) + kBcSFields * nbc;
    const std::size_t off_sp_b = off_sp + (ns_ > 0 ? (ns + 1) * nc : 0);
    const std::size_t off_sp_c =
        off_sp_b + kSpLedgers * ns * sp_groups + 4 * nbc * ns + 2 * np * ns + (ns_ > 0 ? np : 0);
    const std::size_t off_sp_acc = off_sp_c + 5 * ns * nc;
    const std::size_t off_sp_in = off_sp_acc + (ns_ > 0 ? (2 * ns + 1) * ne : 0);
    const std::size_t sp_end =
        off_sp_in + (ns_ > 0 ? (nbc + sp_nodes + 1) * ns + (order2_ ? ns * nc : 0) : 0);
    params_.dispersion = ns_ > 0 ? static_cast<float>(opts.dispersion) : 0.0f;
    params_.off_sp = static_cast<std::uint32_t>(off_sp);
    params_.ns = static_cast<std::uint32_t>(ns);
    params_.temp_row = state.transport.temp_row >= 0
                           ? static_cast<std::uint32_t>(state.transport.temp_row)
                           : kSpNoRow;
    params_.age_row = state.transport.age_row >= 0
                          ? static_cast<std::uint32_t>(state.transport.age_row)
                          : kSpNoRow;
    params_.off_sp_b = static_cast<std::uint32_t>(off_sp_b);
    params_.off_sp_c = static_cast<std::uint32_t>(off_sp_c);
    params_.off_sp_acc = static_cast<std::uint32_t>(off_sp_acc);
    params_.off_sp_in = static_cast<std::uint32_t>(off_sp_in);
    params_.sp_groups = static_cast<std::uint32_t>(sp_groups);
    sp_a_.assign(ns_ > 0 ? (ns + 1) * nc : 0, 0.0f);
    sp_csrc_.assign(ns * nc, 0.0f);
    sp_in_.assign(ns_ > 0 ? (nbc + sp_nodes + 1) * ns : 0, 0.0f);
    sp_b_.assign(off_sp_c - off_sp_b, 0.0f);
    sp_exch_mass_.assign(np * ns, 0.0);
    sp_exch_spill_.assign(ns_ > 0 ? np : 0, 0.0);
    // FULL_SWE's dispersive depth is over the centroid crest zface; the face offsets are to the
    // VFR_FACE crest (the lower endpoint bed) under VFR_FACE: each face's difference, in FP64.
    std::vector<float> sp_zf;
    if (ns_ > 0 && vfr_face) {
        sp_zf.resize(ne);
        for (std::size_t e = 0; e < ne; ++e)
            sp_zf[e] = static_cast<float>(edges_.ze_lo[e] - edges_.zface[e]);
    }
    ctl_ = Ctl{};

    forcing_.assign(4 * nc, 0.0f); // the device's rates are zero until the first upload
    forcing_next_.assign(4 * nc, 0.0f);
    sinks_ = false;
    perf_ = Perf{};
    conveyance_.assign(ne, 0.0f);
    vol_units_.assign(2 * nc, 0u);
    upload_carry_.assign(nc, 0.0);
    pins_.assign(nc, 0u);
    for (const int c : bc_cell_)
        pins_[static_cast<std::size_t>(c)] = 1u;
    // Exchange cells too: their forcing changes at the fastest cadence (== the CPU pin_t0_).
    for (std::size_t k = 0; k < np; ++k)
        if (cp_i[kCpCell * np + k] != kCpNoCell)
            pins_[cp_i[kCpCell * np + k]] = 1u;
    pub_cells_.assign(8 * nc, 0u);
    pub_flux_.assign(ne, 0.0f);
    pub_head_.assign(vfr ? nc : 0, 0.0f);
    pub_q_.assign(swe_ ? 2 * nc : 0, 0.0f);
    // [2D_INITIAL_VELOCITY] (== the other marchers, t = 0 only): FULL_SWE seeds the cell momentum
    // h u; the other schemes seed each open face with the mean of its cells' h u on its normal.
    std::vector<float> init_q;
    {
        bool any_uv = false;
        for (std::size_t i = 0; i < nc && !any_uv; ++i)
            any_uv = mesh.tri_init_u[i] != 0.0 || mesh.tri_init_v[i] != 0.0;
        if (any_uv && swe_) {
            init_q.resize(2 * nc);
            for (std::size_t i = 0; i < nc; ++i) {
                init_q[i] = static_cast<float>(state.depth[i] * mesh.tri_init_u[i]);
                init_q[nc + i] = static_cast<float>(state.depth[i] * mesh.tri_init_v[i]);
            }
        } else if (any_uv) {
            init_q.resize(ne);
            for (std::size_t e = 0; e < ne; ++e) {
                const auto a = static_cast<std::size_t>(edges_.cL[e]);
                const auto c = static_cast<std::size_t>(edges_.cR[e]);
                const double qx =
                    state.depth[a] * mesh.tri_init_u[a] + state.depth[c] * mesh.tri_init_u[c];
                const double qy =
                    state.depth[a] * mesh.tri_init_v[a] + state.depth[c] * mesh.tri_init_v[c];
                init_q[e] = edges_.conveyance[e] > 0.0
                                ? static_cast<float>(0.5 * (qx * edges_.nx[e] + qy * edges_.ny[e]))
                                : 0.0f;
            }
        }
    }
    audit_ = Audit{};
    for (std::size_t i = 0; i < nc; ++i) {
        const std::int64_t units = toUnits(state.volume[i]);
        const std::uint64_t u = static_cast<std::uint64_t>(units);
        vol_units_[i] = static_cast<std::uint32_t>(u);
        vol_units_[nc + i] = static_cast<std::uint32_t>(u >> 32);
        audit_.initial += units;
    }
    audit_.published = audit_.initial;

    const std::uint64_t pub_cells_bytes = 8 * nc * 4, pub_flux_bytes = 4 * std::uint64_t(ne);
    const std::uint64_t face_s_bytes = std::uint64_t(sp_end) * 4; // through the transport rows
    const std::uint64_t face_u_bytes =
        (kFaceUFields * ne + kBcUFields * nbc + 3 * np + 4 * std::size_t(ncp_nodes_)) * 4;
    const std::uint64_t pub_bc_bytes = (std::size_t(kBcSFields) + kBcUFields) * nbc * 4 +
                                       (vfr ? nc * 4 : 0) + 2 * np * 4 + (swe_ ? 2 * nc * 4 : 0) +
                                       (sp_a_.size() + sp_b_.size()) * 4;
    if (std::max<std::uint64_t>({kCellUFields * nc * 4, cell_g.size() * 4, face_g.size() * 4,
                                 idx.size() * 4, face_s_bytes, face_u_bytes}) >
        dev_.limits.maxStorageBufferBindingSize)
        throw std::invalid_argument("BACKEND WEBGPU: the mesh is larger than this GPU's storage "
                                    "buffer limit; use BACKEND CPU for this model.");
    if (std::max(groups(params_.nc, kWorkgroup), groups(params_.ne, kWorkgroup)) >
        dev_.limits.maxComputeWorkgroupsPerDimension)
        throw std::invalid_argument("BACKEND WEBGPU: the mesh needs more workgroups than this "
                                    "GPU dispatches; use BACKEND CPU for this model.");

    buf_ = std::make_unique<Buffers>();
    Buffers& b = *buf_;
    std::string scope_error;
    dev_.run([&](const Done& done) {
        WGPUDevice d = dev_.device;
        wgpuDevicePushErrorScope(d, WGPUErrorFilter_OutOfMemory);
        wgpuDevicePushErrorScope(d, WGPUErrorFilter_Validation);
        const auto storage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        const auto rw = storage | WGPUBufferUsage_CopySrc;
        b.params = makeBuffer(d, sizeof(Params), WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                              "params");
        b.ctl = makeBuffer(d, sizeof(Ctl), rw, "ctl");
        b.cell_g = makeBuffer(d, cell_g.size() * 4, storage, "cell_g");
        b.face_g = makeBuffer(d, face_g.size() * 4, storage, "face_g");
        b.idx = makeBuffer(d, idx.size() * 4, storage, "idx");
        // cellF: the cell fields, order-2 data and vertex bounds, the physics work arrays.
        const std::uint64_t ph_f =
            physics_ ? 24 * std::uint64_t(nc) + 4 * std::uint64_t(ne) + (nc + 255) / 256 : 0;
        b.cell_f = makeBuffer(d, (std::uint64_t(params_.off_ph_f) + ph_f) * 4, rw, "cell_f");
        const std::uint64_t ph_u = physics_ ? 7 * std::uint64_t(nc) + ne + (nc + 255) / 256 : 0;
        b.cell_u = makeBuffer(d, (kCellUFields * std::uint64_t(nc) + ph_u) * 4, rw, "cell_u");
        b.face_s = makeBuffer(d, face_s_bytes, rw, "face_s");
        b.face_u = makeBuffer(d, face_u_bytes, rw, "face_u");
        b.pub_bytes = sizeof(Ctl) + pub_cells_bytes + pub_flux_bytes + pub_bc_bytes;
        b.pub_stage = makeBuffer(d, b.pub_bytes, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
                                 "pub_stage");
        WGPUBuffer bound[9] = {b.params, b.ctl,    b.cell_g, b.face_g, b.idx,
                               b.cell_f, b.cell_u, b.face_s, b.face_u};
        WGPUBindGroupEntry entries[9];
        for (int k = 0; k < 9; ++k) {
            entries[k] = WGPU_BIND_GROUP_ENTRY_INIT;
            entries[k].binding = static_cast<std::uint32_t>(k);
            entries[k].buffer = bound[k];
            entries[k].size = wgpuBufferGetSize(bound[k]);
        }
        WGPUBindGroupDescriptor gd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        gd.layout = dev_.layout;
        gd.entryCount = 9;
        gd.entries = entries;
        b.group = wgpuDeviceCreateBindGroup(d, &gd);
        // Buffers start zeroed (WebGPU guarantees it); write what is not zero.
        wgpuQueueWriteBuffer(dev_.queue, b.params, 0, &params_, sizeof(Params));
        wgpuQueueWriteBuffer(dev_.queue, b.ctl, 0, &ctl_, sizeof(Ctl));
        wgpuQueueWriteBuffer(dev_.queue, b.cell_g, 0, cell_g.data(), cell_g.size() * 4);
        if (!face_g.empty())
            wgpuQueueWriteBuffer(dev_.queue, b.face_g, 0, face_g.data(), face_g.size() * 4);
        wgpuQueueWriteBuffer(dev_.queue, b.idx, 0, idx.data(), idx.size() * 4);
        wgpuQueueWriteBuffer(dev_.queue, b.cell_u, 0, vol_units_.data(), vol_units_.size() * 4);
        wgpuQueueWriteBuffer(dev_.queue, b.cell_u, std::uint64_t(kPin) * nc * 4, pins_.data(),
                             pins_.size() * 4);
        if (!sp_zf.empty())
            wgpuQueueWriteBuffer(dev_.queue, b.face_s,
                                 (std::uint64_t(params_.off_sp_acc) + 2 * ns * ne) * 4,
                                 sp_zf.data(), sp_zf.size() * 4);
        if (swe_ && !init_q.empty())
            wgpuQueueWriteBuffer(dev_.queue, b.cell_f, std::uint64_t(kQcx) * nc * 4, init_q.data(),
                                 init_q.size() * 4);
        else if (!init_q.empty())
            wgpuQueueWriteBuffer(dev_.queue, b.face_s, 0, init_q.data(), init_q.size() * 4);
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(d, nullptr);
        WGPUComputePassEncoder pass = dev_.beginPass(enc, b.group);
        wgpuComputePassEncoderSetPipeline(pass, dev_.pipe[int(Pipe::ReconstructAll)]);
        wgpuComputePassEncoderDispatchWorkgroups(pass, groups(params_.nc, kWorkgroup), 1, 1);
        if (!swe_ && !init_q.empty() && perot_) {
            // The Perot vectors of the seeded face discharges.
            wgpuComputePassEncoderSetPipeline(pass, dev_.pipe[int(Pipe::RefreshPerot)]);
            wgpuComputePassEncoderDispatchWorkgroups(pass, groups(params_.nc, kWorkgroup), 1, 1);
        }
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
        submit(dev_, enc);
        struct Pops {
            std::string* error;
            Done done;
            int left;
        };
        auto* pops = new Pops{&scope_error, done, 2};
        WGPUPopErrorScopeCallbackInfo info = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
        info.mode = WGPUCallbackMode_AllowSpontaneous;
        info.callback = [](WGPUPopErrorScopeStatus, WGPUErrorType type, WGPUStringView msg,
                           void* u1, void*) {
            auto* p = static_cast<Pops*>(u1);
            if (type != WGPUErrorType_NoError && p->error->empty())
                *p->error = text(msg);
            if (--p->left == 0) {
                p->done();
                delete p;
            }
        };
        info.userdata1 = pops;
        wgpuDevicePopErrorScope(d, info);
        wgpuDevicePopErrorScope(d, info);
    });
    if (!scope_error.empty()) {
        finalize();
        throw std::runtime_error("WebGPU 2D: " + scope_error);
    }
    hostClosure();
    span_ = 0.0;
    cycles_since_rebuild_ = 1000;
    last_dt_ = 0.0;
    last_steps_ = substeps_run_ = face_passes_ = 0;
    tier_occupancy_.fill(0);
    initialized_ = true;
    std::fprintf(stderr,
                 "[openswmm 2D] WebGPU marcher: %d cells, %d faces; f32 dynamics, exact integer "
                 "water (2^-30 m3 units).\n",
                 nc_, ne_);
    if (ns_ > 0)
        std::fprintf(stderr,
                     "[openswmm 2D] WebGPU marcher: %d transport rows in f32 mass; aging, decay "
                     "and reactions run on the host.\n",
                     ns_);
}

void WebGpuSurfaceSolver::hostClosure() {
    // The engine's own FP64 closure of the exact volume (== the Kokkos CellClosure dispatch).
    auto& s = *state_;
    const MeshData& m = *mesh_;
    const bool vfr = opts_->cell_closure == CellClosure2D::VFR;
    const double mwf = opts_->vfr_min_wet_frac;
    const int workers = std::max(1, opts_->num_threads);
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers)
#endif
    for (int i = 0; i < nc_; ++i) {
        const std::size_t u = static_cast<std::size_t>(i);
        double eta, depth;
        if (vfr && m.cell_vertex_count(i) == 4)
            inertial::etaDepthQuadScalar(m.tri_area[u], m.tri_cz[u], &m.quad_vfr_z[u * kQuadVfrZ],
                                         m.quad_vfr_a[2 * u], m.quad_vfr_a[2 * u + 1], true, mwf,
                                         s.volume[u], eta, depth);
        else
            inertial::etaDepthScalar(
                m.tri_area[u], m.tri_cz[u], m.vz[m.cell_v[u * kMaxCellVerts + 0]],
                m.vz[m.cell_v[u * kMaxCellVerts + 1]], m.vz[m.cell_v[u * kMaxCellVerts + 2]], vfr,
                mwf, s.volume[u], eta, depth);
        s.head[u] = eta;
        s.depth[u] = depth;
    }
}

void WebGpuSurfaceSolver::uploadVolumes() {
    const std::size_t nc = static_cast<std::size_t>(nc_);
    std::int64_t total = 0;
    const int workers = std::max(1, opts_->num_threads);
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers) reduction(+ : total)
#endif
    for (std::size_t i = 0; i < nc; ++i) {
        // A host edit (an outfall transfer) is rounded to whole units here. Each cell carries
        // its remainder into the next upload: a steady discharge rounds the same way every
        // routing step, and without the carry that bias grows with the run.
        const double exact = std::ldexp(state_->volume[i], kUnitExponent) + upload_carry_[i];
        const std::int64_t units = std::llround(exact);
        upload_carry_[i] = exact - static_cast<double>(units);
        const std::uint64_t u = static_cast<std::uint64_t>(units);
        vol_units_[i] = static_cast<std::uint32_t>(u);
        vol_units_[nc + i] = static_cast<std::uint32_t>(u >> 32);
        total += units;
    }
    audit_.external += total - audit_.published; // host edits (outfall transfers, hot start)
    audit_.published = total;
    dev_.run([&](const Done& done) {
        wgpuQueueWriteBuffer(dev_.queue, buf_->cell_u, 0, vol_units_.data(), vol_units_.size() * 4);
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev_.device, nullptr);
        WGPUComputePassEncoder pass = dev_.beginPass(enc, buf_->group);
        wgpuComputePassEncoderSetPipeline(pass, dev_.pipe[int(Pipe::ReconstructAll)]);
        wgpuComputePassEncoderDispatchWorkgroups(pass, groups(params_.nc, kWorkgroup), 1, 1);
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
        submit(dev_, enc);
        done();
    });
    hostClosure();
}

bool WebGpuSurfaceSolver::pushForcings() {
    const auto& s = *state_;
    const std::size_t nc = static_cast<std::size_t>(nc_);
    auto at = [](const std::vector<double>& v, std::size_t i) { return i < v.size() ? v[i] : 0.0; };
    const bool rain = s.rainfall.size() == nc, evap = s.evap_rate.size() == nc;
    // The rates the CPU marcher resolves (snow liquid share, snow-exposed evaporation and the
    // runtime overrides); the wet/dry ramps and availability limits run on the device.
    float* const out = forcing_next_.data();
    const int workers = std::max(1, opts_->num_threads);
    if (s.runtime_forcings.rows.empty() && !s.snow.active() && rain && evap &&
        s.coupling_flux.size() == nc && s.infil_rate.size() == nc) {
        // No snow and no runtime overrides: the held arrays are the rates.
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers)
#endif
        for (std::size_t i = 0; i < nc; ++i) {
            out[i] = static_cast<float>(s.rainfall[i]);
            out[nc + i] = static_cast<float>(s.coupling_flux[i]);
            out[2 * nc + i] = static_cast<float>(s.evap_rate[i]);
            out[3 * nc + i] = static_cast<float>(s.infil_rate[i]);
        }
    } else {
        for (std::size_t i = 0; i < nc; ++i) {
            const int c = static_cast<int>(i);
            out[i] = static_cast<float>(rain ? s.liquidRainfall(c) : 0.0);
            out[nc + i] = static_cast<float>(at(s.coupling_flux, i));
            out[2 * nc + i] = static_cast<float>(evap ? s.exposedEvaporation(c) : 0.0);
            out[3 * nc + i] =
                static_cast<float>(s.runtime_forcings.rate(3, c, at(s.infil_rate, i)));
        }
    }
    // Branch-free: a coupling, evaporation or infiltration rate means ledgers to read back.
    int sinks = 0;
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers) reduction(| : sinks)
#endif
    for (std::size_t i = 0; i < nc; ++i)
        sinks |= (out[nc + i] != 0.0f) | (out[2 * nc + i] > 0.0f) | (out[3 * nc + i] > 0.0f);
    sinks_ = sinks;
    // Boundary types, slopes, stages and discharges as the router resolved them for this
    // advance (series and rating curves included). A stage is passed relative to the cell's
    // reference bed, formed in FP64.
    if (nbc_ > 0) {
        const auto& bnd = *s.boundary;
        const std::size_t nbc = static_cast<std::size_t>(nbc_);
        for (std::size_t k = 0; k < nbc; ++k) {
            const auto slot = static_cast<std::size_t>(bc_slot_[k]);
            bc_type_[k] = static_cast<std::uint32_t>(bnd.edge_bc_type[slot]);
            bc_slope_[k] = static_cast<float>(bnd.edge_bed_slope[slot]);
            bc_drive_[k] = static_cast<float>(bnd.edge_bc_head[slot] -
                                              zref_[static_cast<std::size_t>(bc_cell_[k])]);
            bc_drive_[nbc + k] = static_cast<float>(bnd.edge_bc_flow[slot]);
            bc_drive_[2 * nbc + k] = static_cast<float>(
                slot < mesh_->edge_conveyance.size() ? mesh_->edge_conveyance[slot] : 1.0);
        }
    }
    pushNodeState();
    // Rates are held between gauge updates: upload them only when they change (compared in
    // chunks on the engine's threads).
    const std::size_t n = forcing_.size(), chunk = std::size_t(1) << 16;
    const long chunks = static_cast<long>((n + chunk - 1) / chunk);
    int differs = 0;
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers) reduction(| : differs)
#endif
    for (long c = 0; c < chunks; ++c) {
        const std::size_t at = std::size_t(c) * chunk, len = std::min(chunk, n - at);
        differs |= std::memcmp(forcing_next_.data() + at, forcing_.data() + at, len * 4) != 0;
    }
    if (!differs)
        return false;
    forcing_.swap(forcing_next_);
    return true;
}

void WebGpuSurfaceSolver::pushNodeState() {
    if (ncp_ == 0)
        return;
    // The routing step's held 1D state (== the Kokkos pushNodeState): each point's node head
    // relative to its datum and node depth, its COUPLING_EQUALIZE factor (pond area over the
    // window; 0 = no cap), and each node's spill budget, its ponded volume in integer units.
    // The budget is the pond the node's depth holds when the router publishes it
    // (node_spill_volume, as the CPU and Kokkos marchers read it): the dynamic wave books a
    // ponded node's volume as a running sum that drifts from that pond.
    const auto& s = *state_;
    const auto& nodes = *s.nodes_1d;
    const double len = opts_->len_1d_to_2d, vol = opts_->vol_1d_to_2d;
    const std::size_t np = static_cast<std::size_t>(ncp_);
    const double window = s.node_pond_area_m2 ? s.coupling_window_s : 0.0;
    for (std::size_t k = 0; k < np; ++k) {
        const auto ni = static_cast<std::size_t>(cp_node_[k]);
        cp_drive_[k] = static_cast<float>(nodes.head[ni] * len - cp_zp_[k]);
        cp_drive_[np + k] = static_cast<float>(nodes.depth[ni] * len);
        double eq = 0.0;
        if (window > 0.0 && ni < s.node_pond_area_m2->size() && (*s.node_pond_area_m2)[ni] > 0.0)
            eq = (*s.node_pond_area_m2)[ni] / window;
        cp_drive_[2 * np + k] = static_cast<float>(eq);
    }
    const std::size_t nn = node_slot_node_.size();
    for (std::size_t n = 0; n < nn; ++n) {
        const auto ni = static_cast<std::size_t>(node_slot_node_[n]);
        const double base = s.node_pond_base ? (*s.node_pond_base)[ni] : nodes.full_volume[ni];
        const double ponded = (s.node_spill_volume ? (*s.node_spill_volume)[ni]
                                                   : std::max(0.0, nodes.volume[ni] - base)) *
                              vol;
        const auto u = static_cast<std::uint64_t>(std::max<std::int64_t>(0, toUnits(ponded)));
        cp_budget_[n] = static_cast<std::uint32_t>(u);
        cp_budget_[nn + n] = static_cast<std::uint32_t>(u >> 32);
    }
}

bool WebGpuSurfaceSolver::refreshConveyance() {
    // == InertialEdges::refreshConveyance, on the engine's threads (it scans every edge each
    // routing step), keeping the f32 copy the device reads.
    const auto& factors = mesh_->edge_conveyance;
    const int workers = std::max(1, opts_->num_threads);
    int changed = 0;
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers) reduction(| : changed)
#endif
    for (int e = 0; e < ne_; ++e) {
        const auto slot = static_cast<std::size_t>(edges_.slotL[e]);
        const double factor = slot < factors.size() ? factors[slot] : 1.0;
        changed |= edges_.conveyance[e] != factor;
        edges_.conveyance[e] = factor;
        conveyance_[e] = static_cast<float>(factor);
    }
    return changed != 0;
}

double WebGpuSurfaceSolver::advance(double t_current, double t_target) {
    if (!initialized_ || t_target <= t_current)
        return t_target;
    const double t_begin = emscripten_get_now();
    const double span = t_target - t_current;
    const bool conveyance = refreshConveyance();
    const double t_forcing = emscripten_get_now();
    perf_.conveyance_ms += t_forcing - t_begin;
    const bool forcing = pushForcings();
    pushSpecies();
    perf_.forcing_ms += emscripten_get_now() - t_forcing;
    params_.span = static_cast<float>(span);
    ctl_.thi = ctl_.tlo = ctl_.sync_hi = ctl_.sync_lo = 0.0f;
    ctl_.mode = kModeMarch;
    ctl_.tail_left = 0;
    ctl_.cycles = cycles_since_rebuild_;
    ctl_.steps = ctl_.face_steps = 0;
    ctl_.finishing = ctl_.finished = 0;
    std::fill(std::begin(ctl_.occupancy), std::end(ctl_.occupancy), 0u);
    ctl_.sp_binds = 0;
    const std::size_t nc = static_cast<std::size_t>(nc_), ne = static_cast<std::size_t>(ne_);
    Buffers& b = *buf_;
    const std::uint32_t gc = groups(params_.nc, kWorkgroup), gf = groups(params_.ne, kWorkgroup);
    // Read back: the control block, the volumes, the applied-source ledgers while a sink or
    // coupling rate is set (they are zero otherwise), and the published edge flux.
    // The junction exchange books into the coupling ledger.
    const bool ledgers = sinks_ || ncp_ > 0;
    const std::uint64_t ctl_bytes = sizeof(Ctl), vol_bytes = 2 * nc * 4;
    const std::uint64_t led_bytes = ledgers ? 6 * nc * 4 : 0, flux_bytes = 4 * std::uint64_t(ne);
    const std::size_t nbc = static_cast<std::size_t>(nbc_);
    const std::uint64_t bcs_bytes = kBcSFields * nbc * 4, bcu_bytes = kBcUFields * nbc * 4;
    const std::uint32_t gr = groups(params_.nruns, kWorkgroup);
    const std::size_t np = static_cast<std::size_t>(ncp_);
    const std::uint32_t gx = groups(params_.ncp_groups, kWorkgroup);
    const std::uint32_t gv = groups(params_.nv, kWorkgroup);
    // VFR: the device's local heads too (the host would otherwise rerun the Newton closure).
    const std::uint64_t head_bytes = vfr_ ? nc * 4 : 0;
    // The volume each junction point exchanged this advance (it accumulates over the rounds).
    const std::uint64_t cp_bytes = 2 * np * 4;
    // FULL_SWE: the cell momentum, for the published velocity.
    const std::uint64_t q_bytes = swe_ ? 2 * nc * 4 : 0;
    // Transport rows: the cell mass and runoff, and the round's totals (which drain).
    const std::uint64_t spa_bytes = sp_a_.size() * 4, spb_bytes = sp_b_.size() * 4;
    const std::uint64_t read_bytes = ctl_bytes + vol_bytes + led_bytes + flux_bytes + bcs_bytes +
                                     bcu_bytes + head_bytes + cp_bytes + q_bytes + spa_bytes +
                                     spb_bytes;
    perf_.prep_ms += emscripten_get_now() - t_begin;

    // One submission per round: a batch of cycles, then the end-of-advance sequence (settle,
    // rebuild or lazy sources, flux publication; gated on the device so it runs only once the
    // march has landed) and the copies back. A good estimate lands in the first round.
    // The per-advance inputs (rates, conveyance, boundary and junction values).
    auto writeInputs = [&]() {
        if (forcing)
            wgpuQueueWriteBuffer(dev_.queue, b.cell_g, kRain * nc * 4, forcing_.data(),
                                 forcing_.size() * 4);
        if (conveyance && ne > 0)
            wgpuQueueWriteBuffer(dev_.queue, b.face_g, kConv * ne * 4, conveyance_.data(), ne * 4);
        if (nbc > 0) {
            wgpuQueueWriteBuffer(dev_.queue, b.idx,
                                 (std::uint64_t(params_.off_bc_idx) + nbc + nruns_ + 1) * 4,
                                 bc_type_.data(), nbc * 4);
            wgpuQueueWriteBuffer(dev_.queue, b.face_g,
                                 (std::uint64_t(params_.off_bc_g) + kBcSlope * nbc) * 4,
                                 bc_slope_.data(), nbc * 4);
            wgpuQueueWriteBuffer(dev_.queue, b.face_g,
                                 (std::uint64_t(params_.off_bc_g) + kBcStage * nbc) * 4,
                                 bc_drive_.data(), bc_drive_.size() * 4);
        }
        if (np > 0) {
            wgpuQueueWriteBuffer(dev_.queue, b.face_g,
                                 (std::uint64_t(params_.off_cp_g) + kCpH1 * np) * 4,
                                 cp_drive_.data(), cp_drive_.size() * 4);
            wgpuQueueWriteBuffer(dev_.queue, b.idx,
                                 (std::uint64_t(params_.off_cp_idx) + kCpIFields * np +
                                  params_.ncp_groups + 1 + np + params_.nst) *
                                     4,
                                 cp_budget_.data(), cp_budget_.size() * 4);
        }
        if (ns_ > 0) {
            // The rows as the router left them (aging, decay, reactions, washoff), their
            // source concentrations and the coupling source density.
            wgpuQueueWriteBuffer(dev_.queue, b.face_s, std::uint64_t(params_.off_sp) * 4,
                                 sp_a_.data(), spa_bytes);
            wgpuQueueWriteBuffer(dev_.queue, b.face_s, std::uint64_t(params_.off_sp_in) * 4,
                                 sp_in_.data(), sp_in_.size() * 4);
            if (params_.sp_flags & kSpFlagCsrc)
                wgpuQueueWriteBuffer(
                    dev_.queue, b.face_s,
                    (std::uint64_t(params_.off_sp_c) + kSpLedgers * std::uint64_t(ns_) * nc) * 4,
                    sp_csrc_.data(), sp_csrc_.size() * 4);
        }
    };
    bool inputs_done = false;
    if (physics_) {
        // Momentum physics: the inputs, then the host-driven global steps; the rounds below
        // only finish the advance (settle, lazy sources, publication).
        dev_.run([&](const Done& done) {
            wgpuQueueWriteBuffer(dev_.queue, b.params, 0, &params_, sizeof(Params));
            writeInputs();
            WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev_.device, nullptr);
            if (nbc > 0)
                wgpuCommandEncoderClearBuffer(
                    enc, b.face_s, (std::uint64_t(params_.off_bc_s) + kBcAccum * nbc) * 4, nbc * 4);
            submit(dev_, enc);
            done();
        });
        inputs_done = true;
        physicsMarch(span);
    }
    bool first = true;
    long cycles_recorded = 0;
    int stalls = 0;
    do {
        const double rem = span - (double(ctl_.thi) + double(ctl_.tlo));
        // Macro cycles of nsub base steps, plus one for the tail.
        const double cycle = double(ctl_.dt0) * params_.nsub;
        const double estimate = ctl_.mode == kModeTail ? 1.0
                                : ctl_.dt0 > 0.0f      ? std::ceil(1.05 * rem / cycle) + 1.0
                                                       : 8.0;
        const int batch = static_cast<int>(std::clamp(estimate, 1.0, 256.0));
        const float thi = ctl_.thi, tlo = ctl_.tlo;
        const std::uint32_t steps = ctl_.steps;
        const double t_round = emscripten_get_now();
        dev_.run([&](const Done& done) {
            if (first) {
                // Later rounds continue from the device's own control block.
                wgpuQueueWriteBuffer(dev_.queue, b.ctl, 0, &ctl_, sizeof(Ctl));
                wgpuQueueWriteBuffer(dev_.queue, b.params, 0, &params_, sizeof(Params));
            }
            if (first && !inputs_done)
                writeInputs();
            WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev_.device, nullptr);
            if (first && !inputs_done && nbc > 0) // the boundary flux accumulates over one advance
                wgpuCommandEncoderClearBuffer(
                    enc, b.face_s, (std::uint64_t(params_.off_bc_s) + kBcAccum * nbc) * 4, nbc * 4);
            if (first && !inputs_done && np > 0) // exchanged and drawn volumes are per advance
                wgpuCommandEncoderClearBuffer(enc, b.face_u, std::uint64_t(params_.off_cp_u) * 4,
                                              (2 * np + 2 * std::uint64_t(ncp_nodes_)) * 4);
            WGPUComputePassEncoder pass = dev_.beginPass(enc, b.group);
            auto go = [&](Pipe k, std::uint32_t n) {
                wgpuComputePassEncoderSetPipeline(pass, dev_.pipe[int(k)]);
                wgpuComputePassEncoderDispatchWorkgroups(pass, n, 1, 1);
            };
            // Group 1 for the next dispatches: base substep `sub`, selector `filter` (S.y).
            auto sub_at = [&](std::uint32_t sub, std::uint32_t filter) {
                const std::uint32_t offset = dev_.subOffset(sub, filter);
                wgpuComputePassEncoderSetBindGroup(pass, 1, dev_.sub_group, 1, &offset);
            };
            // Settle, lazy sources and the active-set and tier rebuild (each kernel acts only
            // when the device's plan asked for it): halo, FRONT_REBUILD rings and frontier, the
            // CFL minimum, tiers, the dry-halo tier pull-down, face tiers.
            auto rebuild = [&]() {
                go(Pipe::Sync, gc);
                go(Pipe::Halo, gc);
                if (front_) {
                    for (std::uint32_t ring = 1; ring < kFrontHaloRings; ++ring) {
                        sub_at(0, 1);
                        go(Pipe::Ring, gc);
                        sub_at(0, 2);
                        go(Pipe::Ring, gc);
                    }
                    go(Pipe::Frontier, gc);
                }
                go(Pipe::Cfl, gc);
                go(Pipe::TierAssign, gc);
                if (front_ && params_.tiers > 1) {
                    for (std::uint32_t round = 0; round < kPullRounds; ++round) {
                        sub_at(0, round % 2 == 0 ? 1 : 2);
                        go(Pipe::Pull, gc);
                    }
                    go(Pipe::TierCount, gc);
                }
                go(Pipe::FaceTier, gf);
            };
            if (first && conveyance) {
                // refreshConveyance: close faces whose factor fell to zero, refresh the Perot
                // vectors from the new factors.
                go(Pipe::CloseFaces, gf);
                if (perot_) {
                    go(Pipe::RefreshPerot, gc);
                    if (nruns_ > 0)
                        go(Pipe::RefreshBoundaryPerot, gr);
                }
            }
            go(Pipe::Plan, 1);
            for (int k = 0; k < batch; ++k) {
                rebuild();
                go(Pipe::Control, 1);
                if (order2_) {
                    // RECONSTRUCTION_ORDER 2 (one tier): the cycle is one SSP-RK2 step, two
                    // Euler stages from the limited gradients, then the exact average.
                    sub_at(0, 0);
                    go(Pipe::RkSnapshot, gc);
                    for (int stage = 0; stage < 2; ++stage) {
                        if (np > 0) {
                            // The junction rates at the stage's start, applied after its cells
                            // and boundary at half weight (== the other marchers).
                            sub_at(0, 3);
                            go(Pipe::Exchange, gx);
                            sub_at(0, 0);
                        }
                        go(Pipe::VertexBounds, gv);
                        go(Pipe::Gradients, gc);
                        go(Pipe::Faces, gf);
                        go(Pipe::Cells, gc);
                        if (nruns_ > 0)
                            go(Pipe::Boundary, gr);
                        if (np > 0) {
                            sub_at(0, 4);
                            go(Pipe::Exchange, gx);
                            sub_at(0, 0);
                        }
                    }
                    go(Pipe::RkAverage, gc);
                }
                for (std::uint32_t sub = 0; sub < (order2_ ? 0u : params_.nsub); ++sub) {
                    // Faces due at this base substep, then cells, then the boundary edges and
                    // the junction exchange. With exchange points the tier-0 cells fire first,
                    // then the boundary and the exchange, then the slower tiers (== Kokkos).
                    sub_at(sub, np > 0 ? 1u : 0u);
                    go(Pipe::Faces, gf);
                    go(Pipe::Cells, gc);
                    if (nruns_ > 0)
                        go(Pipe::Boundary, gr);
                    if (np > 0) {
                        go(Pipe::Exchange, gx);
                        sub_at(sub, 2u);
                        go(Pipe::Cells, gc);
                    }
                }
                go(Pipe::AdvanceClock, 1); // also plans the next cycle
            }
            go(Pipe::FinishPlan, 1);
            rebuild();
            go(Pipe::FinishControl, 1);
            go(Pipe::PublishFlux, gf);
            if (ns_ > 0)
                go(Pipe::SpFold, gc); // the rows' per-cell ledgers into the round's partials
            wgpuComputePassEncoderEnd(pass);
            wgpuComputePassEncoderRelease(pass);
            std::uint64_t at = 0;
            wgpuCommandEncoderCopyBufferToBuffer(enc, b.ctl, 0, b.pub_stage, at, ctl_bytes);
            at += ctl_bytes;
            wgpuCommandEncoderCopyBufferToBuffer(enc, b.cell_u, 0, b.pub_stage, at, vol_bytes);
            at += vol_bytes;
            if (ledgers) {
                // The ledgers drain at every read (after the copy; folded on the host).
                wgpuCommandEncoderCopyBufferToBuffer(enc, b.cell_u, vol_bytes, b.pub_stage, at,
                                                     led_bytes);
                wgpuCommandEncoderClearBuffer(enc, b.cell_u, vol_bytes, led_bytes);
                at += led_bytes;
            }
            if (flux_bytes)
                wgpuCommandEncoderCopyBufferToBuffer(enc, b.face_s, ne * 4, b.pub_stage, at,
                                                     flux_bytes);
            at += flux_bytes;
            if (nbc > 0) {
                // Boundary state, and the receipts, which drain at every read like the ledgers.
                wgpuCommandEncoderCopyBufferToBuffer(
                    enc, b.face_s, std::uint64_t(params_.off_bc_s) * 4, b.pub_stage, at, bcs_bytes);
                at += bcs_bytes;
                wgpuCommandEncoderCopyBufferToBuffer(
                    enc, b.face_u, std::uint64_t(params_.off_bc_u) * 4, b.pub_stage, at, bcu_bytes);
                wgpuCommandEncoderClearBuffer(enc, b.face_u, std::uint64_t(params_.off_bc_u) * 4,
                                              bcu_bytes);
                at += bcu_bytes;
            }
            if (head_bytes)
                wgpuCommandEncoderCopyBufferToBuffer(enc, b.cell_f, std::uint64_t(kHead) * nc * 4,
                                                     b.pub_stage, at, head_bytes);
            at += head_bytes;
            if (cp_bytes)
                wgpuCommandEncoderCopyBufferToBuffer(
                    enc, b.face_u, std::uint64_t(params_.off_cp_u) * 4, b.pub_stage, at, cp_bytes);
            at += cp_bytes;
            if (q_bytes)
                wgpuCommandEncoderCopyBufferToBuffer(enc, b.cell_f, std::uint64_t(kQcx) * nc * 4,
                                                     b.pub_stage, at, q_bytes);
            at += q_bytes;
            if (spa_bytes) {
                wgpuCommandEncoderCopyBufferToBuffer(
                    enc, b.face_s, std::uint64_t(params_.off_sp) * 4, b.pub_stage, at, spa_bytes);
                at += spa_bytes;
                wgpuCommandEncoderCopyBufferToBuffer(
                    enc, b.face_s, std::uint64_t(params_.off_sp_b) * 4, b.pub_stage, at, spb_bytes);
                wgpuCommandEncoderClearBuffer(enc, b.face_s, std::uint64_t(params_.off_sp_b) * 4,
                                              spb_bytes);
            }
            submit(dev_, enc);
            mapThen(
                dev_, b.pub_stage, read_bytes,
                [&](const void* p) {
                    const double t_copy = emscripten_get_now();
                    const auto* bytes = static_cast<const unsigned char*>(p);
                    std::memcpy(&ctl_, bytes, ctl_bytes);
                    std::memcpy(pub_cells_.data(), bytes + ctl_bytes, vol_bytes + led_bytes);
                    const unsigned char* at = bytes + ctl_bytes + vol_bytes + led_bytes;
                    std::memcpy(pub_flux_.data(), at, flux_bytes);
                    std::memcpy(pub_bc_s_.data(), at + flux_bytes, bcs_bytes);
                    std::memcpy(pub_bc_u_.data(), at + flux_bytes + bcs_bytes, bcu_bytes);
                    std::memcpy(pub_head_.data(), at + flux_bytes + bcs_bytes + bcu_bytes,
                                head_bytes);
                    std::memcpy(pub_cp_u_.data(),
                                at + flux_bytes + bcs_bytes + bcu_bytes + head_bytes, cp_bytes);
                    std::memcpy(pub_q_.data(),
                                at + flux_bytes + bcs_bytes + bcu_bytes + head_bytes + cp_bytes,
                                q_bytes);
                    const unsigned char* sp =
                        at + flux_bytes + bcs_bytes + bcu_bytes + head_bytes + cp_bytes + q_bytes;
                    std::memcpy(sp_a_.data(), sp, spa_bytes);
                    std::memcpy(sp_b_.data(), sp + spa_bytes, spb_bytes);
                    perf_.copy_ms += emscripten_get_now() - t_copy;
                },
                done);
        });
        perf_.gpu_ms += emscripten_get_now() - t_round;
        ++perf_.rounds;
        perf_.bytes += read_bytes;
        first = false;
        cycles_recorded += batch;
        const double t_fold = emscripten_get_now();
        if (ledgers)
            foldLedgers();
        foldBoundaryReceipts();
        foldSpecies();
        perf_.fold_ms += emscripten_get_now() - t_fold;
        if (ctl_.n_active_last > 0 && !(std::isfinite(ctl_.dt0) && ctl_.dt0 > 0.0f))
            throw std::runtime_error("WebGPU 2D: the time step became non-finite or zero "
                                     "(dt0 = " +
                                     std::to_string(ctl_.dt0) + " s)");
        if (!ctl_.finished && ctl_.thi == thi && ctl_.tlo == tlo && ctl_.steps == steps &&
            ++stalls >= 3)
            throw std::runtime_error("WebGPU 2D: the marcher stopped advancing");
        if (cycles_recorded > 50'000'000L)
            throw std::runtime_error("WebGPU 2D: more than 5e7 cycles in one advance");
    } while (!ctl_.finished);

    const double t_host = emscripten_get_now();
    publish(span);
    const double t_rows = emscripten_get_now();
    publishSpecies();
    perf_.pub_rows_ms += emscripten_get_now() - t_rows;
    perf_.host_ms += emscripten_get_now() - t_host;
    ++perf_.advances;
    return t_target;
}

void WebGpuSurfaceSolver::physicsMarch(double span) {
    // Momentum physics (== ExplicitKokkosSurfaceSolver::runMomentumStep, applyMomentumPhysics,
    // physicsAdvance): one global step at a time, the host taking each decision from one
    // reduction read back. The kernels stay on the device.
    Buffers& b = *buf_;
    const SolverOptions2D& o = *opts_;
    const std::uint32_t gc = groups(params_.nc, kWorkgroup), gf = groups(params_.ne, kWorkgroup);
    const std::uint32_t gr = groups(params_.nruns, kWorkgroup), gv = groups(params_.nv, kWorkgroup);
    const std::uint32_t gw = groups(params_.nwd_groups, kWorkgroup);
    const double f = o.coriolisParameter();
    double cs = 0, dc = 0;
    const double bg = o.eddy_viscosity_background;
    bool mixing = false, bound_mixing = false;
    if (o.turbulence == Turbulence2D::SMAGORINSKY) {
        cs = o.smagorinsky_cs;
        mixing = cs != 0 || bg != 0;
        bound_mixing = cs > 0 || bg > 0;
    } else if (o.turbulence == Turbulence2D::MIXING_LENGTH) {
        dc = o.mixing_length_coefficient;
        mixing = dc != 0 || bg != 0;
        bound_mixing = dc > 0 || bg > 0;
    }
    ctl_.ph_cs = static_cast<float>(cs);
    ctl_.ph_bg = static_cast<float>(bg);
    ctl_.ph_dc = static_cast<float>(dc);
    auto bitsToFloat = [](std::uint32_t u) {
        float x;
        std::memcpy(&x, &u, 4);
        return x;
    };
    // One submission: write the whole control block or just its physics parameters, record,
    // read the control block back.
    using Record = std::function<void(WGPUComputePassEncoder)>;
    // Host edits to the control block outside its physics parameters (the clock, counters,
    // dt0) go out with the next submission; every submission reads the block back.
    bool dirty = false;
    auto run = [&](bool whole, const Record& record, int what = 0) {
        whole = whole || dirty;
        dirty = false;
        const double t0 = emscripten_get_now();
        dev_.run([&](const Done& done) {
            if (whole)
                wgpuQueueWriteBuffer(dev_.queue, b.ctl, 0, &ctl_, sizeof(Ctl));
            else
                wgpuQueueWriteBuffer(dev_.queue, b.ctl, offsetof(Ctl, ph_cos), &ctl_.ph_cos, 8 * 4);
            WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev_.device, nullptr);
            WGPUComputePassEncoder pass = dev_.beginPass(enc, b.group);
            record(pass);
            wgpuComputePassEncoderEnd(pass);
            wgpuComputePassEncoderRelease(pass);
            wgpuCommandEncoderCopyBufferToBuffer(enc, b.ctl, 0, b.pub_stage, 0, sizeof(Ctl));
            submit(dev_, enc);
            mapThen(
                dev_, b.pub_stage, sizeof(Ctl),
                [&](const void* p) { std::memcpy(&ctl_, p, sizeof(Ctl)); }, done);
        });
        ++perf_.rounds;
        perf_.ph_ms[what] += emscripten_get_now() - t0;
        ++perf_.ph_runs[what];
        if (ctl_.ph_bad)
            throw std::runtime_error("WebGPU 2D: nonfinite mixing viscosity or invalid mixing "
                                     "face geometry.");
    };
    auto go = [&](WGPUComputePassEncoder pass, Pipe k, std::uint32_t n) {
        if (n == 0)
            return;
        wgpuComputePassEncoderSetPipeline(pass, dev_.pipe[int(k)]);
        wgpuComputePassEncoderDispatchWorkgroups(pass, n, 1, 1);
    };
    auto sel = [&](WGPUComputePassEncoder pass, std::uint32_t selector, std::uint32_t round = 0) {
        const std::uint32_t offset = dev_.subOffset(round, selector);
        wgpuComputePassEncoderSetBindGroup(pass, 1, dev_.sub_group, 1, &offset);
    };
    // The eddy viscosity and stress of a velocity source (0: the momentum, into r; 1: the
    // predictor's, into s), and the source's kinetic energy. The corner stencils settle in
    // parallel rounds (S.x the round), then the serial remainder (S.x = kChainRounds + 1).
    auto evaluate = [&](WGPUComputePassEncoder pass, std::uint32_t source) {
        go(pass, Pipe::PhReset, 1);
        go(pass, Pipe::PhConnected, gf);
        sel(pass, source);
        go(pass, Pipe::PhStencil, gc);
        for (std::uint32_t round = 0; round < kChainRounds; ++round) {
            sel(pass, source, round);
            go(pass, Pipe::PhChain, gc);
        }
        go(pass, Pipe::PhSingleFlags, groups((params_.nc + 255) / 256, kWorkgroup));
        sel(pass, source, kChainRounds + 1);
        go(pass, Pipe::PhSingle, 1);
        sel(pass, source);
        go(pass, Pipe::PhFaceStress, gf);
        go(pass, Pipe::PhGather, gc);
        sel(pass, 0);
    };
    auto energy = [&](WGPUComputePassEncoder pass, std::uint32_t source) {
        sel(pass, source);
        go(pass, Pipe::PhEnergyPart, gc);
        go(pass, Pipe::PhEnergySum, 1);
        sel(pass, 0);
    };
    auto rateBound = [&] {
        const double rate = bitsToFloat(ctl_.ph_rate_bits);
        return rate > 0 ? 0.4 / rate : std::numeric_limits<double>::infinity();
    };
    // Heun sub-steps of the eddy-viscosity stress over `seconds` (physicsAdvance), each accepted
    // only if the kinetic energy does not rise beyond kMixingEnergyTolerance (f32 sums).
    auto mix = [&](double seconds) {
        if (seconds <= 0 || !mixing)
            return;
        double elapsed = 0;
        bool accept = false;
        while (elapsed < seconds) {
            run(
                false,
                [&](WGPUComputePassEncoder pass) {
                    if (accept)
                        go(pass, Pipe::PhAccept, gc);
                    evaluate(pass, 0);
                    energy(pass, 0);
                },
                1);
            accept = false;
            double dt = std::min(seconds - elapsed, rateBound());
            const double e0 = ctl_.ph_energy;
            bool accepted = false;
            for (int retry = 0; retry < 40; ++retry) {
                ctl_.ph_dt = static_cast<float>(dt);
                run(
                    false,
                    [&](WGPUComputePassEncoder pass) {
                        go(pass, Pipe::PhPredict, gc);
                        evaluate(pass, 1);
                    },
                    2);
                if (dt <= rateBound()) {
                    run(
                        false,
                        [&](WGPUComputePassEncoder pass) {
                            go(pass, Pipe::PhCorrect, gc);
                            energy(pass, 1);
                        },
                        3);
                    const double e1 = ctl_.ph_energy;
                    if (std::isfinite(e1) &&
                        e1 <= e0 + kMixingEnergyTolerance * std::max(e0, 1e-30)) {
                        accepted = true;
                        break;
                    }
                }
                dt *= 0.5;
            }
            if (!accepted || elapsed + dt == elapsed)
                throw std::runtime_error("WebGPU 2D: Smagorinsky mixing cannot find an admissible "
                                         "timestep.");
            accept = true;
            elapsed = dt == seconds - elapsed ? seconds : elapsed + dt;
        }
        run(false, [&](WGPUComputePassEncoder pass) { go(pass, Pipe::PhAccept, gc); });
    };
    // Half drag, half rotation, mixing, half rotation, half drag (applyMomentumPhysics).
    auto physics = [&](double seconds) {
        const double angle = 0.5 * seconds * f;
        ctl_.ph_seconds = static_cast<float>(0.5 * seconds);
        ctl_.ph_cos = static_cast<float>(std::cos(angle));
        ctl_.ph_sin = static_cast<float>(std::sin(angle));
        const bool drag = params_.nwd_groups > 0, rotate = angle != 0;
        if (drag || rotate)
            run(false, [&](WGPUComputePassEncoder pass) {
                if (drag)
                    go(pass, Pipe::PhWallDrag, gw);
                if (rotate)
                    go(pass, Pipe::PhRotate, gc);
            });
        mix(seconds);
        if (drag || rotate)
            run(false, [&](WGPUComputePassEncoder pass) {
                if (rotate)
                    go(pass, Pipe::PhRotate, gc);
                if (drag)
                    go(pass, Pipe::PhWallDrag, gw);
            });
    };
    double t = 0.0;
    while (t < span) {
        // Rebuild or refresh, then the hydrodynamic CFL step dt0 (plan, rebuild passes, control).
        ctl_.thi = static_cast<float>(std::floor(t));
        ctl_.tlo = static_cast<float>(t - std::floor(t));
        ctl_.mode = kModeMarch;
        ctl_.tail_left = 0;
        ctl_.finishing = ctl_.finished = 0;
        run(
            true,
            [&](WGPUComputePassEncoder pass) {
                go(pass, Pipe::Plan, 1);
                go(pass, Pipe::Sync, gc);
                go(pass, Pipe::Halo, gc);
                if (front_) {
                    for (std::uint32_t ring = 1; ring < kFrontHaloRings; ++ring) {
                        sel(pass, 1);
                        go(pass, Pipe::Ring, gc);
                        sel(pass, 2);
                        go(pass, Pipe::Ring, gc);
                    }
                    sel(pass, 0);
                    go(pass, Pipe::Frontier, gc);
                }
                go(pass, Pipe::Cfl, gc);
                go(pass, Pipe::TierAssign, gc);
                go(pass, Pipe::FaceTier, gf);
                go(pass, Pipe::Control, 1);
            },
            6);
        const float dt0 = ctl_.dt0;
        if (!(std::isfinite(dt0) && dt0 > 0.0f))
            throw std::runtime_error("WebGPU 2D: the time step became non-finite or zero");
        double dt = std::min(double(dt0), span - t);
        if (f != 0)
            dt = std::min(dt, 0.25 / std::abs(f));
        if (bound_mixing) {
            run(false, [&](WGPUComputePassEncoder pass) { evaluate(pass, 0); });
            dt = std::min(dt, rateBound());
        }
        // Trials: the first half of the physics, then the wave CFL of the result.
        run(false, [&](WGPUComputePassEncoder pass) { go(pass, Pipe::PhSave, gc); });
        for (int trial = 0;; ++trial) {
            run(false, [&](WGPUComputePassEncoder pass) { go(pass, Pipe::PhRestore, gc); });
            physics(0.5 * dt);
            run(false, [&](WGPUComputePassEncoder pass) {
                go(pass, Pipe::PhReset, 1);
                go(pass, Pipe::PhWaveCfl, gc);
            });
            const double stable =
                std::min(o.max_timestep, double(bitsToFloat(ctl_.ph_stable_bits)));
            if (dt <= stable)
                break;
            if (trial == 39) {
                run(false, [&](WGPUComputePassEncoder pass) { go(pass, Pipe::PhRestore, gc); });
                throw std::runtime_error("WebGPU 2D: optional momentum physics cannot satisfy "
                                         "the wave CFL.");
            }
            dt = std::min(0.5 * dt, stable);
        }
        // The hydrodynamic step (one tier: faces, cells, boundary; or one RK2 step).
        ctl_.mode = kModeMarch;
        ctl_.do_fire = 1;
        ctl_.do_rebuild = ctl_.do_lazy = 0;
        ctl_.dt0 = ctl_.dt = static_cast<float>(dt);
        run(true, [&](WGPUComputePassEncoder pass) {
            sel(pass, 0);
            if (order2_) {
                go(pass, Pipe::RkSnapshot, gc);
                for (int stage = 0; stage < 2; ++stage) {
                    go(pass, Pipe::VertexBounds, gv);
                    go(pass, Pipe::Gradients, gc);
                    go(pass, Pipe::Faces, gf);
                    go(pass, Pipe::Cells, gc);
                    if (nruns_ > 0)
                        go(pass, Pipe::Boundary, gr);
                }
                go(pass, Pipe::RkAverage, gc);
            } else {
                go(pass, Pipe::Faces, gf);
                go(pass, Pipe::Cells, gc);
                if (nruns_ > 0)
                    go(pass, Pipe::Boundary, gr);
            }
        });
        ctl_.dt0 = dt0; // the hydrodynamic CFL step, which later refreshes only tighten
        ctl_.do_fire = 0;
        ctl_.cycles += 1;
        ctl_.steps += order2_ ? 2u : 1u;
        ctl_.last_dt = static_cast<float>(dt);
        dirty = true;
        t = dt == span - t ? span : t + dt;
        physics(0.5 * dt);
    }
    // Landed: the rounds that follow settle, apply the lazy sources and publish.
    ctl_.thi = static_cast<float>(std::floor(span));
    ctl_.tlo = static_cast<float>(span - std::floor(span));
    ctl_.mode = kModeLanded;
    ctl_.finishing = ctl_.finished = 0;
}

void WebGpuSurfaceSolver::foldLedgers() {
    auto& s = *state_;
    const std::size_t nc = static_cast<std::size_t>(nc_);
    const bool infil = s.infil_applied.size() == nc, coupling = s.coupling_applied.size() == nc;
    std::int64_t evap_total = 0, inf_total = 0, cpl_total = 0;
    const int workers = std::max(1, opts_->num_threads);
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers)                                     \
    reduction(+ : evap_total, inf_total, cpl_total)
#endif
    for (std::size_t i = 0; i < nc; ++i) {
        const std::int64_t inf = cellUnits(kInfLo, i), cpl = cellUnits(kCpLo, i);
        if (infil)
            s.infil_applied[i] += fromUnits(inf) / mesh_->tri_area[i];
        if (coupling)
            s.coupling_applied[i] += fromUnits(cpl);
        evap_total += cellUnits(kEvapLo, i);
        inf_total += inf;
        cpl_total += cpl;
    }
    audit_.infiltration += inf_total;
    audit_.coupling += cpl_total;
    s.evap_loss_total += fromUnits(evap_total);
    audit_.evaporation += evap_total;
}

void WebGpuSurfaceSolver::foldBoundaryReceipts() {
    auto& s = *state_;
    const std::size_t nbc = static_cast<std::size_t>(nbc_);
    auto units = [&](std::uint32_t field, std::size_t k) {
        return static_cast<std::int64_t>(std::uint64_t(pub_bc_u_[field * nbc + k]) |
                                         (std::uint64_t(pub_bc_u_[(field + 1) * nbc + k]) << 32));
    };
    // Additive until the router drains them, as the Kokkos marcher keeps them.
    for (std::size_t k = 0; k < nbc; ++k) {
        s.boundary_in_applied[k] += fromUnits(units(kBcInLo, k));
        s.boundary_out_applied[k] += fromUnits(units(kBcOutLo, k));
        audit_.boundary_in += units(kBcInLo, k);
        audit_.boundary_out += units(kBcOutLo, k);
        s.boundary_requested[k] += fromUnits(units(kBcReqLo, k));
        s.boundary_last_flow[k] = pub_bc_s_[kBcLastFlow * nbc + k];
        s.boundary_last_dt[k] = pub_bc_s_[kBcLastDt * nbc + k];
    }
}

void WebGpuSurfaceSolver::pushSpecies() {
    // == the Kokkos pushSpecies: the rows, rain, coupling, boundary and node concentrations and
    // the runoff volumes, for one advance.
    if (ns_ == 0)
        return;
    const auto& tr = state_->transport;
    if (state_->heat.enabled || state_->live_passive_exchange)
        throw std::runtime_error("WebGPU 2D: surface heat exchange and live passive exchange "
                                 "require the CPU marcher.");
    const std::size_t ns = static_cast<std::size_t>(ns_), nc = static_cast<std::size_t>(nc_);
    const std::size_t nbc = static_cast<std::size_t>(nbc_), nn = node_slot_node_.size();
    const int workers = std::max(1, opts_->num_threads);
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers)
#endif
    for (std::size_t k = 0; k < ns * nc; ++k)
        sp_a_[k] = static_cast<float>(tr.cell_mass[k]);
    const bool runoff = tr.cell_runoff_vol.size() == nc;
    for (std::size_t i = 0; i < nc; ++i)
        sp_a_[ns * nc + i] = runoff ? static_cast<float>(tr.cell_runoff_vol[i]) : 0.0f;
    const bool csrc = tr.coupling_src.size() == ns * nc;
    if (csrc)
        for (std::size_t k = 0; k < ns * nc; ++k)
            sp_csrc_[k] = static_cast<float>(tr.coupling_src[k]);
    float* in = sp_in_.data();
    for (std::size_t k = 0; k < nbc * ns; ++k)
        in[k] = tr.bc_conc.size() == nbc * ns ? static_cast<float>(tr.bc_conc[k]) : 0.0f;
    // The spill concentrations by node slot (the router's per-node rows, frozen for the advance).
    const std::vector<double>* node = state_->node_row_conc;
    const std::size_t n_nodes = state_->nodes_1d ? state_->nodes_1d->volume.size() : 0;
    const bool node_conc = node && n_nodes > 0 && node->size() == n_nodes * ns;
    for (std::size_t n = 0; n < nn; ++n)
        for (std::size_t r = 0; r < ns; ++r)
            in[(nbc + n) * ns + r] =
                node_conc ? static_cast<float>(
                                (*node)[static_cast<std::size_t>(node_slot_node_[n]) * ns + r])
                          : 0.0f;
    for (std::size_t r = 0; r < ns; ++r)
        in[(nbc + nn) * ns + r] =
            r < tr.rain_conc.size() ? static_cast<float>(tr.rain_conc[r]) : 0.0f;
    params_.sp_flags = (csrc ? kSpFlagCsrc : 0u) | (runoff ? kSpFlagRunoff : 0u) |
                       (node_conc ? kSpFlagNodeConc : 0u);
    // Per-advance exchange totals (== the serial marcher's reset).
    std::fill(sp_exch_mass_.begin(), sp_exch_mass_.end(), 0.0);
    std::fill(sp_exch_spill_.begin(), sp_exch_spill_.end(), 0.0);
}

void WebGpuSurfaceSolver::foldSpecies() {
    // One round's row totals, read back and drained, into the cumulative ledgers in slot order
    // (== the Kokkos publishSpecies): the ledger partials in group order, boundary totals in
    // entry order, exchange totals in point order.
    if (ns_ == 0)
        return;
    auto& tr = state_->transport;
    const std::size_t ns = static_cast<std::size_t>(ns_), G = params_.sp_groups;
    const std::size_t nbc = static_cast<std::size_t>(nbc_), np = static_cast<std::size_t>(ncp_);
    auto into = [](std::vector<double>& ledger, std::size_t row, double amount) {
        if (row < ledger.size())
            ledger[row] += amount;
    };
    std::vector<double>* cell_ledgers[kSpLedgers] = {&tr.gained_rainfall, &tr.gained_coupling,
                                                     &tr.lost_infiltration, &tr.lost_coupling};
    const float* p = sp_b_.data();
    for (std::size_t k = 0; k < kSpLedgers; ++k)
        for (std::size_t r = 0; r < ns; ++r) {
            double sum = 0.0;
            for (std::size_t g = 0; g < G; ++g)
                sum += p[(k * ns + r) * G + g];
            into(*cell_ledgers[k], r, sum);
        }
    p += kSpLedgers * ns * G;
    // Boundary entries: gain | loss | requested | applied mass, entry-major.
    for (std::size_t r = 0; r < ns; ++r) {
        double gain = 0.0, loss = 0.0;
        for (std::size_t k = 0; k < nbc; ++k) {
            gain += p[k * ns + r];
            loss += p[(nbc + k) * ns + r];
        }
        into(tr.gained_boundary, r, gain);
        into(tr.lost_boundary, r, loss);
    }
    // Receipts stay additive until the router drains them.
    for (std::size_t k = 0; k < nbc * ns && k < state_->boundary_requested_mass.size(); ++k) {
        state_->boundary_requested_mass[k] += p[2 * nbc * ns + k];
        state_->boundary_applied_mass[k] += p[3 * nbc * ns + k];
    }
    p += 4 * nbc * ns;
    // Junction points: drained mass (for the node) | spill volume | spill mass.
    for (std::size_t r = 0; r < ns; ++r) {
        double drained = 0.0, gained = 0.0;
        for (std::size_t k = 0; k < np; ++k) {
            sp_exch_mass_[k * ns + r] += p[k * ns + r];
            drained += p[k * ns + r];
            gained += p[np * ns + np + k * ns + r];
        }
        into(tr.lost_coupling, r, drained);
        into(tr.gained_coupling, r, gained);
    }
    for (std::size_t k = 0; k < np; ++k)
        sp_exch_spill_[k] += p[np * ns + k];
}

void WebGpuSurfaceSolver::publishSpecies() {
    if (ns_ == 0)
        return;
    auto& tr = state_->transport;
    const std::size_t ns = static_cast<std::size_t>(ns_), nc = static_cast<std::size_t>(nc_);
    const int workers = std::max(1, opts_->num_threads);
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers)
#endif
    for (std::size_t k = 0; k < ns * nc; ++k)
        tr.cell_mass[k] = sp_a_[k];
    if (tr.cell_runoff_vol.size() == nc)
        for (std::size_t i = 0; i < nc; ++i)
            tr.cell_runoff_vol[i] = sp_a_[ns * nc + i];
    // The advance's drained mass per point, which the router hands to the nodes.
    if (tr.exch_mass.size() == sp_exch_mass_.size())
        tr.exch_mass = sp_exch_mass_;
    if (tr.exch_spill.size() == sp_exch_spill_.size())
        tr.exch_spill = sp_exch_spill_;
    tr.dispersion_limiter_binds += ctl_.sp_binds;
}

std::int64_t WebGpuSurfaceSolver::cellUnits(std::uint32_t field, std::size_t i) const {
    const std::size_t nc = static_cast<std::size_t>(nc_);
    return static_cast<std::int64_t>(std::uint64_t(pub_cells_[field * nc + i]) |
                                     (std::uint64_t(pub_cells_[(field + 1) * nc + i]) << 32));
}

void WebGpuSurfaceSolver::publish(double span) {
    auto& s = *state_;
    const std::size_t nc = static_cast<std::size_t>(nc_);
    const double t_volume = emscripten_get_now();
    std::int64_t total = 0;
    const int workers = std::max(1, opts_->num_threads);
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers) reduction(+ : total)
#endif
    for (std::size_t i = 0; i < nc; ++i) {
        const std::int64_t units = cellUnits(kVLo, i);
        s.volume[i] = fromUnits(units);
        total += units;
    }
    audit_.published = total;
    const double t_closure = emscripten_get_now();
    perf_.pub_volume_ms += t_closure - t_volume;
    if (vfr_) {
        // VFR: depth is the exact V/A; the head is the device's local stage on the cell's FP64
        // reference bed (about 1e-6 of the relief from the FP64 closure, which would cost a
        // Newton solve per cell per routing step here).
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers)
#endif
        for (std::size_t i = 0; i < nc; ++i) {
            const double a = mesh_->tri_area[i];
            s.depth[i] = a > 1.0e-30 ? std::max(s.volume[i], 0.0) / a : 0.0;
            s.head[i] = zref_[i] + double(pub_head_[i]);
        }
    } else {
        hostClosure();
    }
    const double t_flux = emscripten_get_now();
    perf_.pub_closure_ms += t_flux - t_closure;
    // Face flux to the two edge slots (-F into cL's slot, +F into cR's), as the other marchers
    // publish. Zero flux (including every face with an inactive cell) stays +0 in both slots;
    // the Kokkos marcher writes -0 into the left slot of an active zero-flux face.
    const long nslots = static_cast<long>(s.edge_flux.size());
    double* const flux = s.edge_flux.data();
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers)
#endif
    for (long k = 0; k < nslots; ++k)
        flux[k] = 0.0;
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers)
#endif
    for (int e = 0; e < ne_; ++e) {
        const double f = pub_flux_[e];
        if (f == 0.0)
            continue;
        s.edge_flux[static_cast<std::size_t>(edges_.slotL[e])] = -f;
        s.edge_flux[static_cast<std::size_t>(edges_.slotR[e])] = f;
    }
    // Boundary edges publish their mean flux over the advance (== the Kokkos publish_bc).
    if (span > 0.0)
        for (int k = 0; k < nbc_; ++k)
            s.edge_flux[static_cast<std::size_t>(bc_slot_[k])] =
                pub_bc_s_[kBcAccum * std::size_t(nbc_) + k] / span;
    // Each junction point's exchanged volume, exact (== the integer the cells received).
    const std::size_t np = static_cast<std::size_t>(ncp_);
    for (std::size_t k = 0; k < np; ++k)
        exch_[k] = fromUnits(static_cast<std::int64_t>(std::uint64_t(pub_cp_u_[k]) |
                                                       (std::uint64_t(pub_cp_u_[np + k]) << 32)));
    exch_span_ = span;
    perf_.pub_flux_ms += emscripten_get_now() - t_flux;
    span_ = span;
    cycles_since_rebuild_ = ctl_.cycles;
    last_steps_ = ctl_.steps;
    last_dt_ = ctl_.last_dt;
    substeps_run_ += ctl_.steps;
    face_passes_ += ctl_.face_steps;
    for (std::uint32_t k = 0; k < kMaxTiers; ++k)
        tier_occupancy_[k] += ctl_.occupancy[k];
}

void WebGpuSurfaceSolver::reinitialize(double /*t0*/) {
    if (!initialized_)
        return;
    // External state edit: volumes authoritative; face discharge and pending transfers stale
    // (== the Kokkos marcher, which keeps the Perot vectors).
    dev_.run([&](const Done& done) {
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev_.device, nullptr);
        if (ne_ > 0) {
            wgpuCommandEncoderClearBuffer(enc, buf_->face_s, 0, std::uint64_t(ne_) * 4);
            wgpuCommandEncoderClearBuffer(enc, buf_->face_u, 0, 4 * std::uint64_t(ne_) * 4);
        }
        if (nbc_ > 0)
            wgpuCommandEncoderClearBuffer(enc, buf_->face_s,
                                          (std::uint64_t(params_.off_bc_s) + kBcQ * nbc_) * 4,
                                          std::uint64_t(nbc_) * 4);
        if (swe_) {
            // FULL_SWE: pending momentum and the cell momentum are stale too (== Kokkos).
            wgpuCommandEncoderClearBuffer(enc, buf_->face_s, std::uint64_t(params_.off_mom) * 4,
                                          4 * std::uint64_t(ne_) * 4);
            wgpuCommandEncoderClearBuffer(enc, buf_->cell_f, std::uint64_t(kQcx) * nc_ * 4,
                                          2 * std::uint64_t(nc_) * 4);
            std::fill(pub_q_.begin(), pub_q_.end(), 0.0f);
        }
        submit(dev_, enc);
        done();
    });
    uploadVolumes();
    cycles_since_rebuild_ = 1000;
}

bool WebGpuSurfaceSolver::publishMomentumVelocity() {
    if (!initialized_ || !swe_)
        return false;
    // == the Kokkos marcher (its sign convention included).
    const double dry = opts_->dry_depth;
    const std::size_t nc = static_cast<std::size_t>(nc_);
    for (std::size_t i = 0; i < nc; ++i) {
        const double h = state_->depth[i];
        state_->face_vx[i] = h > dry ? -double(pub_q_[i]) / h : 0;
        state_->face_vy[i] = h > dry ? -double(pub_q_[nc + i]) / h : 0;
    }
    return true;
}

void WebGpuSurfaceSolver::resyncFromVolumes(double t0) {
    syncPinnedVolumes(t0);
    // An arbitrary volume edit can wet an inactive cell: rebuild at the next advance.
    if (initialized_)
        cycles_since_rebuild_ = 1000;
}

void WebGpuSurfaceSolver::syncPinnedVolumes(double /*t0*/) {
    if (!initialized_)
        return;
    uploadVolumes();
}

void WebGpuSurfaceSolver::pinExternalSourceCells(const std::vector<int>& cells) {
    if (!initialized_)
        return;
    for (const int c : cells)
        if (c < 0 || c >= nc_)
            throw std::out_of_range("pinExternalSourceCells: cell outside the mesh");
    for (const int c : cells)
        pins_[static_cast<std::size_t>(c)] = 1u;
    dev_.run([&](const Done& done) {
        wgpuQueueWriteBuffer(dev_.queue, buf_->cell_u, std::uint64_t(kPin) * nc_ * 4, pins_.data(),
                             pins_.size() * 4);
        done();
    });
    cycles_since_rebuild_ = 1000;
}

void WebGpuSurfaceSolver::finalize() {
    if (initialized_ && std::getenv("OPENSWMM_PERF"))
        std::fprintf(stderr,
                     "[PERF-2D-WEBGPU] advances=%ld rounds=%ld prep_ms=%.1f (conveyance %.1f, "
                     "forcing %.1f) gpu_ms=%.1f host_ms=%.1f read_MB=%.1f steps=%ld\n",
                     perf_.advances, perf_.rounds, perf_.prep_ms, perf_.conveyance_ms,
                     perf_.forcing_ms, perf_.gpu_ms, perf_.host_ms, perf_.bytes / 1.0e6,
                     substeps_run_);
    if (initialized_ && std::getenv("OPENSWMM_PERF"))
        std::fprintf(
            stderr,
            "[PERF-2D-WEBGPU] copy-back host ms: read-back memcpy %.1f (in gpu_ms), round "
            "folds %.1f, publish volume %.1f, head/depth %.1f, face flux %.1f, rows %.1f\n",
            perf_.copy_ms, perf_.fold_ms, perf_.pub_volume_ms, perf_.pub_closure_ms,
            perf_.pub_flux_ms, perf_.pub_rows_ms);
    if (initialized_ && physics_ && std::getenv("OPENSWMM_PERF"))
        std::fprintf(stderr,
                     "[PERF-2D-WEBGPU] physics ms(runs): other %.1f(%ld) mix-eval %.1f(%ld) "
                     "mix-predict %.1f(%ld) mix-correct %.1f(%ld) rebuild %.1f(%ld)\n",
                     perf_.ph_ms[0], perf_.ph_runs[0], perf_.ph_ms[1], perf_.ph_runs[1],
                     perf_.ph_ms[2], perf_.ph_runs[2], perf_.ph_ms[3], perf_.ph_runs[3],
                     perf_.ph_ms[6], perf_.ph_runs[6]);
    if (initialized_ && buf_ && std::getenv("OPENSWMM_PERF"))
        writeAudit();
    if (buf_) {
        Buffers& b = *buf_;
        dev_.run([&](const Done& done) {
            if (b.group)
                wgpuBindGroupRelease(b.group);
            b.group = nullptr;
            for (WGPUBuffer* p : {&b.params, &b.ctl, &b.cell_g, &b.face_g, &b.idx, &b.cell_f,
                                  &b.cell_u, &b.face_s, &b.face_u, &b.pub_stage})
                release(*p);
            done();
        });
        buf_.reset();
    }
    initialized_ = false;
}

void WebGpuSurfaceSolver::writeAudit() {
    const std::size_t nc = static_cast<std::size_t>(nc_);
    const std::uint64_t bytes = 2 * nc * 4;
    std::vector<std::uint32_t> rain(2 * nc, 0u);
    Buffers& b = *buf_;
    dev_.run([&](const Done& done) {
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev_.device, nullptr);
        wgpuCommandEncoderCopyBufferToBuffer(enc, b.cell_u, std::uint64_t(kRainLo) * nc * 4,
                                             b.pub_stage, 0, bytes);
        submit(dev_, enc);
        mapThen(
            dev_, b.pub_stage, bytes, [&](const void* p) { std::memcpy(rain.data(), p, bytes); },
            done);
    });
    std::int64_t rain_units = 0;
    for (std::size_t i = 0; i < nc; ++i)
        rain_units +=
            static_cast<std::int64_t>(std::uint64_t(rain[i]) | (std::uint64_t(rain[nc + i]) << 32));
    const Audit& a = audit_;
    const std::int64_t expected = a.initial + a.external + rain_units + a.coupling + a.boundary_in -
                                  a.infiltration - a.evaporation - a.boundary_out;
    std::fprintf(stderr,
                 "[PERF-2D-WEBGPU] water audit (m3): initial %.17g, external %.17g, rain %.17g, "
                 "coupling %.17g, boundary in %.17g, infiltration %.17g, evaporation %.17g, "
                 "boundary out %.17g, final %.17g; residual %lld units (exact: 0)\n",
                 fromUnits(a.initial), fromUnits(a.external), fromUnits(rain_units),
                 fromUnits(a.coupling), fromUnits(a.boundary_in), fromUnits(a.infiltration),
                 fromUnits(a.evaporation), fromUnits(a.boundary_out), fromUnits(a.published),
                 static_cast<long long>(a.published - expected));
}

ISurfaceSolver::RunStats WebGpuSurfaceSolver::run_stats() const noexcept {
    RunStats s;
    s.nsteps = substeps_run_;
    s.nrhs = face_passes_;
    s.last_h = last_dt_;
    s.n_tiers = static_cast<int>(params_.tiers);
    for (int k = 0; k < s.n_tiers; ++k)
        s.tier_cells[k] = tier_occupancy_[static_cast<std::size_t>(k)];
    return s;
}

} // namespace openswmm::twoD::webgpu
