// SPDX-License-Identifier: Apache-2.0
/**
 * @file      WebGpuSurfaceSolver.hpp
 * @brief     WebGPU port of the explicit local-inertial surface marcher (browser builds only).
 *
 * @details Compiled only into the Emscripten threads build with OPENSWMM_ENABLE_WEBGPU, and
 *          selected only by an explicit [2D_OPTIONS] BACKEND WEBGPU (AUTO never picks it).
 *
 *          The marcher follows ExplicitKokkosSurfaceSolver's per-advance contract:
 *          refreshConveyance → pushForcings → march (active-set and tier rebuild
 *          every kRebuildEvery macro cycles, else a tighten-only dt0 refresh; macro
 *          cycles of 2^(LTS_TIERS-1) base substeps; an equal-substep tail with an
 *          exact landing) → settle → lazy sources → publish. The kernels are WGSL
 *          (WebGpuKernels.hpp); every decision of a cycle is taken on the device,
 *          so one submission per routing step records the cycles, the end of the
 *          advance and the copies back, and one readback returns them.
 *
 *          WebGPU objects live on one dedicated GPU pthread. The engine thread
 *          blocks in emscripten_proxy_sync while that thread records, submits
 *          and maps; the mapAsync callback, delivered by the GPU thread's own
 *          event loop, copies the results into engine memory (shared) and
 *          releases the engine thread.
 *
 *          Scope (slices W1-W7 of the P6 program): LOCAL_INERTIAL, DIFFUSIVE_WAVE and FULL_SWE
 *          (first and second order; FRONT_REBUILD, which FULL_SWE turns on by default) with the FLAT or VFR
 *          cell closure and MEAN or VFR_FACE face depth, LTS_TIERS 1-8, triangles and
 *          quads, every boundary type (WALL, NORMAL_FLOW, SPECIFIED_STAGE, SPECIFIED_FLOW,
 *          RATING_CURVE), rain/evaporation/infiltration/coupling-rate sources, the live
 *          1D-2D junction exchange and outfall pins, the optional momentum physics
 *          (Coriolis, turbulence, wall drag; physicsMarch, host-driven global steps with an
 *          f32 mixing acceptance tolerance), and the transport rows (pollutants, MSX species,
 *          water age, passive temperature; f32 mass, dispersion, boundary quality, junction
 *          exchange), whose aging, decay and reactions the router runs on the host.
 *          Everything else is refused: by the factory (options and capability
 *          bits) or by initialize() (model state), never silently substituted.
 *
 *          Precision: f32 dynamics, exact 64-bit integer water, f32 row mass (see
 *          WebGpuKernels.hpp). Results are not bit-identical to the FP64 marchers.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../solver/GpuPluginAbi.h"
#include "../solver/ISurfaceSolver.hpp"
#include "../solver/InertialEdges.hpp"
#include "WebGpuKernels.hpp"

namespace openswmm::twoD {
struct SolverOptions2D;
}

namespace openswmm::twoD::webgpu {

/// Capability bits the WebGPU marcher implements (the factory's gate, as for the Kokkos plugin).
inline constexpr std::uint64_t kWebGpuSurfaceCapabilities =
    OPENSWMM_GPU_CAP_LOCAL_INERTIAL | OPENSWMM_GPU_CAP_DIFFUSIVE_WAVE | OPENSWMM_GPU_CAP_FULL_SWE |
    OPENSWMM_GPU_CAP_SWE_ORDER2 | OPENSWMM_GPU_CAP_QUADS | OPENSWMM_GPU_CAP_FRONT_REBUILD |
    OPENSWMM_GPU_CAP_MOMENTUM_PHYSICS | OPENSWMM_GPU_CAP_TRANSPORT_LI |
    OPENSWMM_GPU_CAP_TRANSPORT_DW | OPENSWMM_GPU_CAP_TRANSPORT_SWE;

/**
 * @brief Name the option settings the WebGPU marcher does not implement yet.
 * @param[in] opts Validated 2D options.
 * @return Comma-separated settings, or empty when the options are within scope. Capability bits
 * (momentum closure, quads, transport, heat, groundwater) are checked separately.
 */
std::string unsupportedOptions(const SolverOptions2D& opts);

/**
 * @brief Create a WebGPU marcher when this browser has a usable WebGPU device.
 * @param[out] why Set to the reason when nullptr is returned.
 * @return The marcher, or nullptr without WebGPU, an adapter or a device. The first call starts
 * the GPU thread and requests the device, blocking the calling thread until it is known.
 */
std::unique_ptr<ISurfaceSolver> makeWebGpuSurfaceSolver(std::string& why);

class Device; // GPU thread, device and pipelines (WebGpuSurfaceSolver.cpp)
struct Buffers;

class WebGpuSurfaceSolver final : public ISurfaceSolver {
public:
    explicit WebGpuSurfaceSolver(Device& device);
    ~WebGpuSurfaceSolver() override;

    /**
     * @brief Upload the mesh, state and options; reconstruct head and depth.
     * @throws std::invalid_argument A model setting outside the W1 scope (named in the message).
     * @throws std::runtime_error A WebGPU error while creating buffers or pipelines.
     */
    void initialize(MeshData& mesh, SurfaceStateData& state, SolverOptions2D& opts) override;
    /**
     * @brief March from @p t_current to @p t_target on the device and publish to the host.
     * @return t_target.
     * @throws std::runtime_error A WebGPU error, or a non-finite or stalled time step.
     * @post state.volume (exact), head and depth (the FP64 cell closure of that volume), edge_flux,
     * and additive infil_applied, coupling_applied and evap_loss_total receipts.
     */
    double advance(double t_current, double t_target) override;
    /// Upload host volumes, clear face discharge and accumulators, rebuild at the next advance.
    void reinitialize(double t0) override;
    /// Upload host volumes keeping face discharge; rebuild at the next advance.
    void resyncFromVolumes(double t0) override;
    /// Upload host volumes (edits of pinned cells only), keeping the active set.
    void syncPinnedVolumes(double t0) override;
    /// Keep the listed cells active; forces a rebuild at the next advance.
    void pinExternalSourceCells(const std::vector<int>& cells) override;
    /// Release the device buffers. Idempotent.
    void finalize() override;

    long last_num_steps() const noexcept override { return last_steps_; }
    double last_step_size() const noexcept override { return last_dt_; }
    /// Per junction point, the exchanged volume of the last advance (m3, exact; + 2D to 1D).
    const std::vector<double>& last_coupling_exchange() const noexcept override { return exch_; }
    double last_coupling_interval() const noexcept override { return exch_span_; }
    /// FULL_SWE: the cell velocity from the cell momentum read back with the last advance.
    bool publishMomentumVelocity() override;
    RunStats run_stats() const noexcept override;
    bool is_initialized() const noexcept override { return initialized_; }

private:
    void uploadVolumes();        ///< state.volume → device integer units; reconstruct
    void hostClosure();          ///< head/depth from state.volume (the engine's FP64 closure)
    bool pushForcings();         ///< resolve held rates (f32); true when they changed
    bool refreshConveyance();    ///< changed face factors → device; true if any changed
    void publish(double span);   ///< device → state (volume, head, depth, flux)
    void foldLedgers();          ///< applied-source ledgers read back → additive receipts
    void foldBoundaryReceipts(); ///< boundary in/out/requested read back → additive receipts
    std::int64_t cellUnits(std::uint32_t field, std::size_t i) const; ///< read-back integer

    Device& dev_;
    std::unique_ptr<Buffers> buf_;
    MeshData* mesh_ = nullptr;
    SurfaceStateData* state_ = nullptr;
    SolverOptions2D* opts_ = nullptr;
    InertialEdges edges_;
    int nc_ = 0, ne_ = 0, nslots_ = 0, nbc_ = 0, nruns_ = 0;
    std::vector<int> bc_cell_, bc_slot_;  ///< non-WALL boundary entries, cell by cell
    std::vector<std::uint32_t> bc_type_;  ///< per entry, resolved per advance
    std::vector<float> bc_slope_;         ///< per entry, resolved per advance
    std::vector<float> bc_drive_;         ///< per entry: local stage | outward q (2·nbc)
    std::vector<float> pub_bc_s_;         ///< boundary state read back (kBcSFields·nbc)
    std::vector<std::uint32_t> pub_bc_u_; ///< boundary receipts read back (kBcUFields·nbc)
    bool perot_ = false;
    double x0_ = 0.0, y0_ = 0.0, z0_ = 0.0; ///< local origin and elevation datum of the f32 data

    Params params_{};
    Ctl ctl_{};
    std::vector<float> forcing_;           ///< rain | coupling | evap | infil (4·nc), on device
    std::vector<float> forcing_next_;      ///< the same, resolved for this advance
    bool sinks_ = false;                   ///< a coupling, evaporation or infiltration rate is set
    std::vector<float> conveyance_;        ///< ne
    std::vector<std::uint32_t> vol_units_; ///< lo | hi (2·nc)
    std::vector<double> upload_carry_;     ///< per cell: host-edit rounding carried (units)
    std::vector<std::uint32_t> pins_;      ///< nc
    std::vector<std::uint32_t> pub_cells_; ///< volume and ledgers, lo | hi pairs (8·nc)
    std::vector<float> pub_flux_;          ///< published face flux (ne)
    std::vector<float> pub_head_;          ///< VFR: the device's local heads (nc)
    std::vector<float> pub_q_;             ///< FULL_SWE: the cell momentum qx | qy (2·nc)
    bool swe_ = false, diffusive_ = false, front_ = false, order2_ = false, physics_ = false;
    void physicsMarch(double span); ///< momentum physics: host-driven global steps to the span
    std::vector<double> zref_;      ///< each cell's reference bed (FP64, mesh datum)
    bool vfr_ = false;              ///< CELL_CLOSURE VFR

    // Junction exchange (non-outfall coupling points; WebGpuKernels.hpp CpI/CpG layout).
    int ncp_ = 0, ncp_groups_ = 0, ncp_nodes_ = 0;
    std::vector<int> cp_node_;             ///< per point: the 1D node index
    std::vector<int> node_slot_node_;      ///< per node slot: the 1D node index
    std::vector<double> cp_zp_;            ///< per point: its datum (the coupling cell's zref)
    std::vector<float> cp_drive_;          ///< per point: 1D head, 1D depth, equalize factor
    std::vector<std::uint32_t> cp_budget_; ///< per node slot: spill budget units (lo | hi)
    std::vector<std::uint32_t> pub_cp_u_;  ///< exchanged units per point read back (lo | hi)
    std::vector<double> exch_;             ///< per point: m3 exchanged in the last advance
    double exch_span_ = 0.0;               ///< the advance exch_ covers (s)
    void pushNodeState();                  ///< held 1D node state → cp_drive_, cp_budget_

    // Transport rows (WebGpuKernels.hpp kSp* regions; f32 mass). The router edits the rows
    // between advances (aging, decay, reactions, washoff), so they cross both ways each advance.
    int ns_ = 0;                        ///< rows carried; 0 for a water-only model
    std::vector<float> sp_a_;           ///< cell mass | runoff volume (the off_sp region)
    std::vector<float> sp_csrc_;        ///< coupling source density (ns nc)
    std::vector<float> sp_in_;          ///< boundary | node-slot | rain concentrations
    std::vector<float> sp_b_;           ///< one round's totals read back (the off_sp_b region)
    std::vector<double> sp_exch_mass_;  ///< per point and row: mass drained this advance
    std::vector<double> sp_exch_spill_; ///< per point: spill volume this advance
    void pushSpecies();                 ///< host rows → sp_a_, sp_csrc_, sp_in_ and the flags
    void foldSpecies();                 ///< one round's totals → the ledgers and receipts
    void publishSpecies();              ///< sp_a_ → cell mass and runoff; the advance's totals

    struct Perf {
        long advances = 0, rounds = 0;
        double prep_ms = 0, conveyance_ms = 0, forcing_ms = 0, gpu_ms = 0, host_ms = 0, bytes = 0;
        /// The host's share of the copy-back: the read-back memcpy (inside gpu_ms), the round
        /// folds, and publish() split into volume, head/depth closure, face flux and the rows.
        double copy_ms = 0, fold_ms = 0, pub_volume_ms = 0, pub_closure_ms = 0, pub_flux_ms = 0,
               pub_rows_ms = 0;
        double ph_ms[8] = {}; ///< momentum physics: ms per submission kind
        long ph_runs[8] = {}; ///< and the count
    } perf_;                  ///< OPENSWMM_PERF summary at finalize

    double span_ = 0.0;
    std::uint32_t cycles_since_rebuild_ = 1000;
    double last_dt_ = 0.0;
    long last_steps_ = 0, substeps_run_ = 0, face_passes_ = 0;
    std::array<long, kMaxTiers> tier_occupancy_{}; ///< cells per tier summed over the rebuilds

    /// Integer water audit (units of 2^-30 m3), checked at finalize under OPENSWMM_PERF: the
    /// published volume must equal initial + external edits + rain + net coupling + boundary in
    /// - infiltration - evaporation - boundary out, exactly.
    struct Audit {
        std::int64_t initial = 0, external = 0, published = 0, coupling = 0, infiltration = 0,
                     evaporation = 0, boundary_in = 0, boundary_out = 0;
    } audit_;
    void writeAudit(); ///< read the applied rain back and print the audit
    bool initialized_ = false;
};

} // namespace openswmm::twoD::webgpu
