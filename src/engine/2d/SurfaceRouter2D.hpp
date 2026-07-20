/**
 * @file SurfaceRouter2D.hpp
 * @brief Top-level orchestrator for the optional 2D surface routing module.
 *
 * @details Manages the full 2D workflow within the engine lifecycle:
 *          - Mesh topology construction (after parse)
 *          - CVODE solver initialization
 *          - Per-step coupling, rainfall update, solver advance
 *          - Statistics and finalization
 *
 *          Integrates with SWMMEngine via lifecycle hooks:
 *          initialize() → step() → finalize()
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §4.1, §8.3
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP
#define OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP

#include "data/MeshData.hpp"
#include "data/SurfaceStateData.hpp"
#include "data/SolverOptions2D.hpp"
#include "data/BoundaryData.hpp"
#include "coupling/NodeCoupling.hpp"
#include "../uncertainty/GridFileReader.hpp"
#include "../uncertainty/SpdeSpatialBasis.hpp"

#ifdef OPENSWMM_HAS_2D
#include "solver/CvodeSurfaceSolver.hpp"
#include "uncertainty/SpatialUncertaintyField.hpp"
#endif

#include <vector>
namespace openswmm::uncertainty { struct SoftGridSourceSpec; }
namespace openswmm::uncertainty { enum class GridMapping : int8_t; }

namespace openswmm {
struct SimulationContext;
}

// Forward declaration outside openswmm::twoD so the fully-qualified name
// resolves to openswmm::uncertainty::SpectralROM1D, not the invalid
// openswmm::twoD::openswmm::uncertainty::SpectralROM1D.
namespace openswmm::uncertainty { struct SpectralROM1D; }

namespace openswmm::twoD {

// Forward declaration so rom() is usable without pulling in SpectralROM.hpp.
struct SpectralROM;

/**
 * @brief Top-level orchestrator for the 2D surface routing module.
 */
class SurfaceRouter2D {
public:
    SurfaceRouter2D() = default;
    ~SurfaceRouter2D() = default;

    // Non-copyable, movable
    SurfaceRouter2D(const SurfaceRouter2D&) = delete;
    SurfaceRouter2D& operator=(const SurfaceRouter2D&) = delete;
    SurfaceRouter2D(SurfaceRouter2D&&) = default;
    SurfaceRouter2D& operator=(SurfaceRouter2D&&) = default;

    /**
     * @brief Initialize the 2D module after input parsing is complete.
     *
     * Builds mesh topology, vertex stencils, resolves coupling names,
     * and initializes the CVODE solver.
     *
     * @param ctx Simulation context (must have mesh_2d populated from parsing).
     */
    void initialize(SimulationContext& ctx);

    /**
     * @brief Advance the 2D surface routing by one SWMM routing step.
     *
     * Sequence:
     * 1. Update outfall boundary heads from 2D state (before 1D routing)
     * 2. After 1D routing: compute coupling exchange flows
     * 3. Update 2D rainfall from system gages
     * 4. Advance CVODE by dt_swmm
     * 5. Transfer outfall discharges into 2D cells
     * 6. Update statistics
     *
     * @param ctx  Simulation context.
     * @param dt   SWMM routing timestep (seconds).
     * @param t    Current simulation time (seconds from start).
     */
    void step(SimulationContext& ctx, double dt, double t);

    /**
     * @brief Pre-routing hook: update outfall boundaries from 2D surface heads.
     *
     * Must be called BEFORE the 1D routing step, after setOutfallDepths().
     *
     * @param ctx Simulation context.
     */
    void updateOutfallsPreRouting(SimulationContext& ctx);

    /**
     * @brief Post-routing hook: compute coupling exchange and advance 2D solver.
     *
     * Must be called AFTER the 1D routing step.
     *
     * @param ctx  Simulation context.
     * @param dt   SWMM routing timestep (seconds).
     * @param t    Current simulation time (seconds from start).
     */
    void advancePostRouting(SimulationContext& ctx, double dt, double t);

    /**
     * @brief Finalize the 2D module at simulation end.
     */
    void finalize();

    /**
     * @brief Compute a CFL-like stability hint for the 2D domain.
     *
     * Returns an advisory maximum dt based on mesh resolution and wave speeds.
     * CVODE handles its own sub-stepping, but this prevents the coupling
     * interval from being too large.
     *
     * @param ctx Simulation context.
     * @return Advisory maximum timestep (seconds).
     */
    double computeCflHint(const SimulationContext& ctx) const;

    /// Check if the 2D module is active.
    bool isActive() const noexcept { return active_; }

    /// Access mesh data (read-only).
    const MeshData& mesh() const noexcept { return mesh_; }

    /// Access mesh data (mutable, for input parsing).
    MeshData& mesh() noexcept { return mesh_; }

    /// Access surface state (read-only).
    const SurfaceStateData& state() const noexcept { return state_; }

    /// Access surface state (mutable, for forcing).
    SurfaceStateData& state() noexcept { return state_; }

    /// Access solver options (read-only).
    const SolverOptions2D& options() const noexcept { return options_; }

    /// Access solver options (mutable).
    SolverOptions2D& options() noexcept { return options_; }

    /// Access per-edge boundary-condition data (read-only).
    const BoundaryData& boundary() const noexcept { return boundary_; }

    /// Access per-edge boundary-condition data (mutable, for forcing/parsing).
    BoundaryData& boundary() noexcept { return boundary_; }

    /**
     * @brief Initialize 2D gridded rainfall forcing from a SoftGridSourceSpec (SR-2c).
     *
     * Opens the HDF5 grid file, builds the CENTROID mapping (pixel index per
     * triangle centroid), and marks the grid as active. Called from
     * SWMMEngine::initialize() when a 2D-target grid source with /location
     * is configured.
     *
     * @param spec  The grid source specification (file path, mapping, etc.).
     * @param inp_dir  Directory of the parent .inp file (for relative path resolution).
     * @return true on success; false on error (call last_error via grid_reader_).
     */
    bool initGridRainfall(const uncertainty::SoftGridSourceSpec& spec,
                          const std::string& inp_dir);

    /// True if 2D gridded rainfall forcing is active (SR-2c).
    bool gridRainfallActive() const noexcept { return grid_2d_active_; }

    /**
     * @brief Per-row buffer for `[2D_BOUNDARY_CONDITIONS]` parse output.
     *
     * V-E3. Populated by the input parser during reading (before the
     * mesh is finalized), drained into `boundary_` during `initialize()`
     * after `boundary_.resize()` allocates the per-edge slots. Cleared
     * after drain.
     */
    struct PendingBoundaryRow {
        int         tri      = 0;
        int         edge     = 0;     ///< 0..2
        int         bc_type  = 0;     ///< openswmm::twoD::BoundaryType cast
        double      param1   = 0.0;   ///< slope (NormalFlow) / head (Stage) / flow (Flow)
        std::string name;             ///< TS name or curve name (TS_/Rating variants)
        std::string group;            ///< named group ("" = none)
    };
    std::vector<PendingBoundaryRow>& pendingBCRows() noexcept { return pending_bc_rows_; }
    const std::vector<PendingBoundaryRow>& pendingBCRows() const noexcept { return pending_bc_rows_; }

    /// Get total 2D surface volume (sum of depth * area).
    double totalVolume() const;

    /// Get total exchange flow (sum of coupling flows, m³/s).
    double totalExchangeFlow() const;

#ifdef OPENSWMM_HAS_2D
    /// Access CVODE solver statistics.
    long lastCvodeSteps() const { return cvode_solver_.last_num_steps(); }
    double lastCvodeStepSize() const { return cvode_solver_.last_step_size(); }
    /// Access the ROM sidecar (null if ROM not active or not yet seeded).
    const SpectralROM* rom() const noexcept { return cvode_solver_.rom(); }

    /**
     * @brief Per-coupling-point exchange flux bounds from the last ROM step.
     *
     * Valid (is_valid() == true) after the first advancePostRouting() call when
     * the ROM sidecar is active and there are coupling points.  Returns a
     * reference to the struct stored inside the ROM; empty when ROM not active.
     */
    const CouplingUncertaintyOutput& couplingOutput() const noexcept {
        static const CouplingUncertaintyOutput empty{};
        const SpectralROM* r = cvode_solver_.rom();
        return r ? r->coupling_unc_output : empty;
    }

    /**
     * @brief Register a 1D network ROM for per-member coupling head reconstruction.
     *
     * Forwarded to CvodeSurfaceSolver::setROM1D().  The caller retains ownership.
     * Pass nullptr to clear.
     */
    void setROM1D(const openswmm::uncertainty::SpectralROM1D* rom1d) noexcept {
        cvode_solver_.setROM1D(rom1d);
    }
#else
    long lastCvodeSteps() const { return 0; }
    double lastCvodeStepSize() const { return 0.0; }
    const SpectralROM* rom() const noexcept { return nullptr; }
    void setROM1D(const openswmm::uncertainty::SpectralROM1D*) noexcept {}
#endif

private:
    MeshData         mesh_;
    SurfaceStateData state_;
    SolverOptions2D  options_;
    BoundaryData     boundary_;

    /// V-E3 — parse-time scratch for [2D_BOUNDARY_CONDITIONS] rows.
    std::vector<PendingBoundaryRow> pending_bc_rows_;

    std::vector<CouplingPoint> coupling_points_;

    bool   active_           = false;
    int    coupling_counter_ = 0;
    double sim_time_         = 0.0;

#ifdef OPENSWMM_HAS_2D
    CvodeSurfaceSolver cvode_solver_;
#endif

    std::vector<double> grid_spread_;       ///< SR-3b: mapped spread plane in model rain units
    bool grid_soft_warned_ = false;         ///< SR-3c: lognormal CV>0.5 warning emitted once

    // CL-1c correlated coherence (COHERENCE CORR_LEN) for the 2D grid path.
    double grid_soft_corr_len_ = 0.0;       ///< Correlation length (m) of the active grid source; 0 ⇒ comonotone
    bool   grid_soft_field_built_ = false;  ///< True once grid_soft_field_ has been generated
#ifdef OPENSWMM_HAS_2D
    SpatialUncertaintyField grid_soft_field_; ///< Static per-member per-cell coefficient field (built once)
    // CL-2c reduced spatial basis (replaces the CL-1c materialized field's
    // O(M·n·n_nbr) generation with a ~ms analytic SPDE build). When K_s < M the
    // ROM uses the reduced projection over grid_soft_psi_ + grid_soft_a_.
    openswmm::uncertainty::SpdeSpatialBasis grid_soft_basis_; ///< SPDE ν=2 basis over triangle centroids
    std::vector<double> grid_soft_psi_;     ///< Normalized mode fields ψ_m(t), K_s × n_tri row-major
    std::vector<double> grid_soft_a_;       ///< Per-member modal coefficients a_im, M × K_s row-major
    bool grid_soft_reduced_ = false;        ///< True when the reduced projection path is active (K_s < M)
#endif

    /// Update rainfall from system rain gages.
    void updateRainfall(SimulationContext& ctx);

    // --- SR-2c: Gridded rainfall forcing (deterministic /location plane) ---
    bool   grid_2d_active_ = false;          ///< True when a 2D-target grid source with /location is active
    GridFileReader grid_reader_;             ///< HDF5 grid file reader (one 2D source in v1)
    std::vector<uint32_t> grid_px_;          ///< CENTROID mapping: pixel index per triangle centroid
    // SR-4a: BILINEAR mapping — 4-pixel weighted gather per triangle centroid.
    // Value-initialized ({}) to 0 == GridMapping::CENTROID (forward-declared
    // enum with int8_t underlying type; set explicitly in initGridRainfall).
    uncertainty::GridMapping grid_mapping_{};
    std::vector<uint32_t> grid_bilin_idx_;   ///< 4 pixel indices per triangle (row-major: [4*i + k])
    std::vector<float>    grid_bilin_w_;     ///< 4 bilinear weights per triangle (sum to 1)
    bool   grid_reader_opened_ = false;      ///< True after grid_reader_.open() succeeded
    double grid_last_time_ = -1.0;           ///< Last grid time plane advanced to
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP
