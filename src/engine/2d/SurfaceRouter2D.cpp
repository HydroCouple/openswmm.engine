/**
 * @file SurfaceRouter2D.cpp
 * @brief Implementation of the 2D surface routing orchestrator.
 *
 * @see SurfaceRouter2D.hpp
 * @ingroup engine_2d
 */

#include "SurfaceRouter2D.hpp"
#include "mesh/MeshBuilder.hpp"
#include "mesh/VertexReconstruction.hpp"
#include "../core/SimulationContext.hpp"
#include "../uncertainty/UncertaintyConfig.hpp"
#include "../uncertainty/GridMappingWeights.hpp"

#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace openswmm::twoD {

void SurfaceRouter2D::initialize(SimulationContext& ctx) {
    // Check if 2D sections were parsed (vertices present)
    if (mesh_.n_vertices() < 3 || mesh_.n_triangles() < 1) {
        active_ = false;
        return;
    }

    // Build mesh topology (neighbours, edge geometry, areas)
    buildMeshTopology(mesh_);

    // Validate mesh
    auto err = validateMesh(mesh_);
    if (!err.empty()) {
        throw std::runtime_error("2D mesh validation failed: " + err);
    }

    // Build pseudo-Laplacian vertex reconstruction stencils
    buildVertexStencils(mesh_);

    // Resolve deferred coupling node names → indices
    for (int v = 0; v < mesh_.n_vertices(); ++v) {
        auto& name = mesh_.vert_coupled_node_name[v];
        if (name.empty()) continue;

        int node_idx = ctx.node_names.find(name);
        if (node_idx >= 0) {
            mesh_.vert_coupled_node[v] = node_idx;
        } else {
            throw std::runtime_error(
                "2D vertex " + std::to_string(v)
                + " coupled to unknown node '" + name + "'");
        }
    }

    for (int t = 0; t < mesh_.n_triangles(); ++t) {
        auto& name = mesh_.tri_coupled_node_name[t];
        if (name.empty()) continue;

        int node_idx = ctx.node_names.find(name);
        if (node_idx >= 0) {
            mesh_.tri_coupled_node[t] = node_idx;
        } else {
            throw std::runtime_error(
                "2D triangle " + std::to_string(t)
                + " coupled to unknown node '" + name + "'");
        }
    }

    // Initialize surface state
    state_.resize(mesh_.n_triangles(), mesh_.n_vertices());

    // Initialize per-edge boundary condition storage (n_triangles * 3 slots,
    // all initialized to WALL with zero head/slope/cum_flux).
    boundary_.resize(mesh_.n_triangles() * 3);

    // V-E3 — drain any [2D_BOUNDARY_CONDITIONS] rows the parser
    // accumulated into boundary_. Out-of-range (tri/edge beyond mesh
    // size) rows are silently skipped — defensive against partial INPs.
    {
        const int n_edges = boundary_.size();
        for (const auto& r : pending_bc_rows_) {
            if (r.tri < 0 || r.tri >= mesh_.n_triangles()) continue;
            if (r.edge < 0 || r.edge > 2) continue;
            const int idx = r.tri * 3 + r.edge;
            if (idx < 0 || idx >= n_edges) continue;
            boundary_.edge_bc_type[idx] = static_cast<int8_t>(r.bc_type);
            switch (static_cast<BoundaryType>(r.bc_type)) {
            case BoundaryType::NORMAL_FLOW:
                boundary_.edge_bed_slope[idx] = r.param1;
                break;
            case BoundaryType::SPECIFIED_STAGE:
                if (!r.name.empty()) {
                    boundary_.edge_bc_tseries_name[idx] = r.name;
                    boundary_.edge_bc_tseries[idx]      = -2;  // deferred resolve
                } else {
                    boundary_.edge_bc_head[idx] = r.param1;
                }
                break;
            case BoundaryType::SPECIFIED_FLOW:
                if (!r.name.empty()) {
                    boundary_.edge_bc_flow_tseries_name[idx] = r.name;
                    boundary_.edge_bc_flow_tseries[idx]      = -2;
                } else {
                    boundary_.edge_bc_flow[idx] = r.param1;
                }
                break;
            case BoundaryType::RATING_CURVE:
                boundary_.edge_bc_rating_curve_name[idx] = r.name;
                boundary_.edge_bc_rating_curve[idx]      = -2;
                break;
            case BoundaryType::WALL:
                break;
            }
        }
        pending_bc_rows_.clear();
    }

    // Set initial heads from ground elevation
    for (int i = 0; i < mesh_.n_triangles(); ++i) {
        state_.head[i] = mesh_.tri_cz[i];
    }

    // Build coupling point descriptors
    coupling_points_ = buildCouplingPoints(mesh_, ctx);

    // Suppress ponding for 2D-coupled nodes. The 2D surface owns the
    // surface storage, so any legacy ponded_area would double-count.
    //
    // C3a: warn (don't fail) when the user-supplied INP marks a coupled
    // node with sur_depth > 0 or ponded_area > 0. The engine still proceeds
    // — the surcharge gate in computeCouplingExchange honours sur_depth
    // (C1+C2), and ponded_area is zeroed below — but the user should know
    // that membership in [2D_VERTEX_NODE_MAP] / [2D_TRIANGLE_NODE_MAP] is
    // the unambiguous "uncapped" flag and the other two attributes have
    // their meaning narrowed to "physical surcharge cap" only.
    // See docs/1D_2D_COUPLING_GATE_REVIEW.md §6 (C3a).
    for (const auto& cp : coupling_points_) {
        if (cp.is_outfall) continue;
        auto ni = static_cast<std::size_t>(cp.node_idx);
        const auto& nname = ctx.node_names.name_of(cp.node_idx);
        if (ctx.nodes.sur_depth[ni] > 0.0) {
            ctx.warnings.push_back(
                "WARNING: 2D-coupled node '" + nname
                + "' has sur_depth > 0 — surcharge gate uses invert + "
                  "full_depth + sur_depth as the spill threshold "
                  "(z_top). Below z_top the orifice ramp is closed in "
                  "both directions; above z_top it opens.");
        }
        if (ctx.nodes.ponded_area[ni] > 0.0) {
            ctx.warnings.push_back(
                "WARNING: 2D-coupled node '" + nname
                + "' has ponded_area > 0 — the 2D mesh owns surface "
                  "storage for coupled nodes; ponded_area is being "
                  "zeroed.");
        }
        // Set ponded area to zero — 2D surface handles excess
        ctx.nodes.ponded_area[ni] = 0.0;
    }

#ifdef OPENSWMM_HAS_2D
    // Initialize CVODE solver
    cvode_solver_.initialize(mesh_, state_, options_);
#endif

    active_ = true;
    coupling_counter_ = 0;
    sim_time_ = 0.0;
}


void SurfaceRouter2D::step(SimulationContext& ctx, double dt, double t) {
    if (!active_) return;

    // Pre-routing: update outfall boundaries from 2D state
    updateOutfallsPreRouting(ctx);

    // Note: 1D routing happens between pre and post hooks in SWMMEngine

    // Post-routing: coupling exchange and 2D advance
    advancePostRouting(ctx, dt, t);
}


void SurfaceRouter2D::updateOutfallsPreRouting(SimulationContext& ctx) {
    if (!active_) return;
    updateOutfallBoundaries(coupling_points_, mesh_, state_, ctx);
}


void SurfaceRouter2D::advancePostRouting(SimulationContext& ctx, double dt,
                                          double t) {
    if (!active_) return;

    // Subcycling support
    coupling_counter_++;
    if (options_.coupling_interval > 0
        && coupling_counter_ < options_.coupling_interval) {
        return;  // Not yet time to advance 2D
    }
    coupling_counter_ = 0;

    // Save 2D state
    state_.save_state();

    // Compute coupling exchange flows
    computeCouplingExchange(coupling_points_, mesh_, state_, ctx, options_, dt);

    // Transfer outfall discharges into 2D cells
    transferOutfallDischarges(coupling_points_, mesh_, state_, ctx, dt);

    // Update rainfall from system gages
    updateRainfall(ctx);

    // Apply 2D forcings
    for (std::size_t i = 0; i < state_.depth.size(); ++i) {
        // Rainfall forcing
        if (state_.rainfall_forced[i] == 1) {
            state_.rainfall[i] = state_.rainfall_force_val[i];
        } else if (state_.rainfall_forced[i] == 2) {
            state_.rainfall[i] += state_.rainfall_force_val[i];
        }
        // Coupling forcing
        if (state_.coupling_forced[i] == 1) {
            state_.coupling_flux[i] = state_.coupling_force_val[i];
        } else if (state_.coupling_forced[i] == 2) {
            state_.coupling_flux[i] += state_.coupling_force_val[i];
        }
    }

#ifdef OPENSWMM_HAS_2D
    // Propagate coupling exchange into the ROM ensemble (per-member orifice physics)
    // before advancing CVODE so all ensemble members see the same drainage sink.
    if (!coupling_points_.empty())
        cvode_solver_.applyCouplingFluxToROM(
            coupling_points_, ctx.nodes.head.data(), mesh_, dt);

    // Advance CVODE by dt
    double t_target = sim_time_ + dt;
    cvode_solver_.advance(sim_time_, t_target);
#endif

    sim_time_ += dt;

    // Update statistics
    state_.update_statistics(mesh_.tri_area, dt);

    // Clear RESET forcings
    state_.clear_reset_forcings();
}


void SurfaceRouter2D::finalize() {
#ifdef OPENSWMM_HAS_2D
    cvode_solver_.finalize();
#endif
    active_ = false;
}


double SurfaceRouter2D::computeCflHint(const SimulationContext& /*ctx*/) const {
    if (!active_) return 1.0e30;

    // Estimate maximum wave speed from maximum depth and minimum cell size
    double max_depth = 0.0;
    double min_dx = 1.0e30;

    int nt = mesh_.n_triangles();
    for (int i = 0; i < nt; ++i) {
        max_depth = std::max(max_depth, state_.depth[i]);

        // Characteristic cell size ~ sqrt(area)
        double dx = std::sqrt(mesh_.tri_area[i]);
        min_dx = std::min(min_dx, dx);
    }

    if (max_depth < options_.dry_depth || min_dx < 1.0e-6) {
        return 1.0e30;  // All dry — no CFL constraint
    }

    // Shallow water wave speed ~ sqrt(g * h)
    double c = std::sqrt(9.80665 * max_depth);
    return min_dx / std::max(c, 1.0e-6);
}


double SurfaceRouter2D::totalVolume() const {
    double vol = 0.0;
    int nt = mesh_.n_triangles();
    for (int i = 0; i < nt; ++i) {
        vol += state_.depth[i] * mesh_.tri_area[i];
    }
    return vol;
}


double SurfaceRouter2D::totalExchangeFlow() const {
    double flow = 0.0;
    int nt = mesh_.n_triangles();
    for (int i = 0; i < nt; ++i) {
        flow += state_.coupling_flux[i] * mesh_.tri_area[i];
    }
    return flow;
}


// ============================================================================
// initGridRainfall (SR-2c) — build CENTROID mapping + open grid file
// ============================================================================

bool SurfaceRouter2D::initGridRainfall(
    const uncertainty::SoftGridSourceSpec& spec,
    const std::string& inp_dir)
{
    // Resolve relative path against the .inp directory
    std::string path = spec.file_path;
    if (!path.empty() && path[0] != '/' && !inp_dir.empty()) {
        path = inp_dir + "/" + path;
    }

    if (!grid_reader_.open(path)) {
        // Failed to open — fall back to gage path silently (warning could be emitted)
        return false;
    }
    grid_reader_opened_ = true;

    int nx = grid_reader_.nx();
    int ny = grid_reader_.ny();
    const auto& x_coords = grid_reader_.x_coords();
    const auto& y_coords = grid_reader_.y_coords();

    // Build CENTROID mapping: for each triangle centroid, find the containing
    // pixel (nearest cell center). Clamp to edge for centroids outside the grid.
    int nt = mesh_.n_triangles();
    grid_px_.resize(static_cast<std::size_t>(nt));

    for (int i = 0; i < nt; ++i) {
        double cx = mesh_.tri_cx[static_cast<std::size_t>(i)];
        double cy = mesh_.tri_cy[static_cast<std::size_t>(i)];

        // Find nearest x index (binary search or linear — grids are small)
        int ix = 0;
        for (int j = 1; j < nx; ++j) {
            if (std::abs(cx - x_coords[static_cast<std::size_t>(j)])
                < std::abs(cx - x_coords[static_cast<std::size_t>(ix)]))
                ix = j;
        }

        // Find nearest y index
        int iy = 0;
        for (int j = 1; j < ny; ++j) {
            if (std::abs(cy - y_coords[static_cast<std::size_t>(j)])
                < std::abs(cy - y_coords[static_cast<std::size_t>(iy)]))
                iy = j;
        }

        // Flat index into (ny, nx) plane — row-major: [iy * nx + ix]
        grid_px_[static_cast<std::size_t>(i)] =
            static_cast<uint32_t>(iy * nx + ix);
    }

    // SR-4a: BILINEAR mapping — 4-pixel weighted gather per triangle centroid.
    // Requires at least a 2x2 grid; degenerate grids fall back to CENTROID.
    grid_mapping_ = spec.mapping;
    if (grid_mapping_ == uncertainty::GridMapping::BILINEAR) {
        if (nx < 2 || ny < 2) {
            grid_mapping_ = uncertainty::GridMapping::CENTROID;  // fall back
        } else {
            grid_bilin_idx_.assign(static_cast<std::size_t>(4 * nt), 0);
            grid_bilin_w_.assign(static_cast<std::size_t>(4 * nt), 0.0f);
            for (int i = 0; i < nt; ++i) {
                double cx = mesh_.tri_cx[static_cast<std::size_t>(i)];
                double cy = mesh_.tri_cy[static_cast<std::size_t>(i)];
                uint32_t idx4[4];
                double w4[4];
                uncertainty::bilinearWeights(cx, cy, x_coords, y_coords, idx4, w4);
                const auto base = static_cast<std::size_t>(4 * i);
                for (int k = 0; k < 4; ++k) {
                    grid_bilin_idx_[base + static_cast<std::size_t>(k)] = idx4[k];
                    grid_bilin_w_[base + static_cast<std::size_t>(k)] =
                        static_cast<float>(w4[k]);
                }
            }
        }
    }

    grid_spread_.assign(static_cast<std::size_t>(nt), 0.0);
    grid_2d_active_ = true;
    return true;
}

// ============================================================================
// updateRainfall — gage fallback + grid-forced path (SR-2c)
// ============================================================================

void SurfaceRouter2D::updateRainfall(SimulationContext& ctx) {
    int nt = mesh_.n_triangles();

    // SR-2c/SR-3b: map the 2D grid source each step. /location overrides the
    // deterministic gage-0 rainfall path when present; /spread is always
    // mapped for soft forcing into the ROM.
    if (grid_2d_active_ && grid_reader_opened_) {
        // Advance the grid reader to the current simulation time
        double t_now = ctx.current_time;
        // If we haven't started advancing yet, do the first advance
        if (!grid_reader_.has_current()) {
            if (!grid_reader_.advance()) return;
        }
        // Advance until the current plane covers t_now.
        while (grid_reader_.has_current() && grid_reader_.spread_next() != nullptr
               && grid_reader_.time_next() < t_now) {
            if (!grid_reader_.advance()) break;
        }

        // Unit conversion factor: grid units → m/s
        double to_m_per_s;
        if (static_cast<int>(ctx.options.flow_units) < 3)
            to_m_per_s = 0.0254 / 3600.0; // in/hr -> m/s
        else
            to_m_per_s = 0.001 / 3600.0;  // mm/hr -> m/s

        int nx = grid_reader_.nx();
        int ny = grid_reader_.ny();
        const float* loc = grid_reader_.location_now();
        const float* spread = grid_reader_.spread_now();

        // SR-4a: gather a grid field value for triangle i under the active
        // mapping. CENTROID = single containing pixel; BILINEAR = 4-pixel
        // weighted sum. Out-of-range pixels contribute zero.
        const auto npix = static_cast<uint32_t>(nx * ny);
        auto gather = [&](const float* field, int i) -> double {
            if (grid_mapping_ == uncertainty::GridMapping::BILINEAR
                && !grid_bilin_idx_.empty()) {
                const auto base = static_cast<std::size_t>(4 * i);
                double acc = 0.0;
                for (int k = 0; k < 4; ++k) {
                    uint32_t px = grid_bilin_idx_[base + static_cast<std::size_t>(k)];
                    if (px < npix)
                        acc += static_cast<double>(grid_bilin_w_[base + static_cast<std::size_t>(k)])
                               * static_cast<double>(field[px]);
                }
                return acc;
            }
            uint32_t px = grid_px_[static_cast<std::size_t>(i)];
            return (px >= npix) ? 0.0 : static_cast<double>(field[px]);
        };

        // Deterministic location override only when /location is present.
        if (loc) {
            for (int i = 0; i < nt; ++i) {
                state_.rainfall[i] = gather(loc, i) * to_m_per_s;
            }
        }

        // Map spread plane into model rain units for the ROM soft-forcing path.
        if (spread) {
            const uint8_t* fcodes = grid_reader_.family_code_now();
            for (int i = 0; i < nt; ++i) {
                double sp = gather(spread, i) * to_m_per_s;

                // SR-4b: for MIXED family, UNIFORM cells use the centered band
                // (2u-1) while the ROM's shared coefficient column uses z_i
                // (NORMAL). Pre-scale UNIFORM cell spreads by the ratio of the
                // UNIFORM coefficient range to the normal coefficient range so
                // the band width is conservatively comparable under the single
                // z_i column. This is a documented v1 approximation; exact
                // per-cell per-member dispatch is the design's deferred cold path.
                // The family code is selected by the CENTROID pixel even under
                // BILINEAR (the family assignment is categorical, not smooth).
                if (fcodes && grid_reader_.family() == GridFamily::MIXED) {
                    uint32_t px = grid_px_[static_cast<std::size_t>(i)];
                    if (px < npix && fcodes[px] == 2) {  // UNIFORM
                        // max|2u-1| ≈ 1.0; max|z| ≈ 3.0 for M=50 — scale up
                        // the UNIFORM spread so z_i · spread_eff ≈ (2u_i-1) · spread
                        // in expectation. The factor 3.0 is max|z| for typical M.
                        sp *= 3.0;
                    }
                }

                grid_spread_[static_cast<std::size_t>(i)] = sp;
            }

#ifdef OPENSWMM_HAS_2D
            if (auto* rom = cvode_solver_.rom()) {
                using openswmm::uncertainty::DistType;
                DistType fam;
                GridFamily gf = grid_reader_.family();
                if (gf == GridFamily::UNIFORM)        fam = DistType::UNIFORM;
                else if (gf == GridFamily::LOGNORMAL)  fam = DistType::LOGNORMAL;
                else                                   fam = DistType::NORMAL;  // MIXED uses NORMAL coefficient
                rom->setSoftForcing(state_.rainfall.empty() ? nullptr : state_.rainfall.data(),
                                    grid_spread_.data(), fam);
            }
#endif

            // SR-3c: warn once when lognormal delta-linearization is likely weak.
            // For MIXED, check LOGNORMAL cells (family_code == 1).
            if (!grid_soft_warned_) {
                const bool check_lognormal =
                    (grid_reader_.family() == GridFamily::LOGNORMAL)
                    || (grid_reader_.family() == GridFamily::MIXED && fcodes);
                if (check_lognormal) {
                    double max_cv = 0.0;
                    for (int i = 0; i < nt; ++i) {
                        double loc_i = state_.rainfall[static_cast<std::size_t>(i)];
                        if (loc_i <= 1.0e-30) continue;
                        if (grid_reader_.family() == GridFamily::MIXED) {
                            uint32_t px = grid_px_[static_cast<std::size_t>(i)];
                            if (fcodes[px] != 1) continue;  // only LOGNORMAL cells
                        }
                        max_cv = std::max(max_cv,
                                          std::abs(grid_spread_[static_cast<std::size_t>(i)] / loc_i));
                    }
                    if (max_cv > 0.5) {
                        ctx.warnings.push_back(
                            "WARNING: soft rainfall LOGNORMAL delta approximation has CV > 0.5 "
                            "(max CV = " + std::to_string(max_cv)
                            + ") for the active 2D grid source"
                            + (grid_reader_.family() == GridFamily::MIXED ? " (MIXED family)" : "")
                            + ".");
                        grid_soft_warned_ = true;
                    }
                }
            }
        } else {
#ifdef OPENSWMM_HAS_2D
            if (auto* rom = cvode_solver_.rom()) rom->clearSoftForcing();
#endif
        }

        if (loc) return;
    }

    // --- Legacy gage-0 fallback (unchanged when no grid source) ---
    int n_gages = ctx.n_gages();

    if (n_gages <= 0) {
        std::fill(state_.rainfall.begin(), state_.rainfall.end(), 0.0);
        return;
    }

    // Phase 1: use first available gage's current rainfall for all cells
    // Future: natural neighbour interpolation across all gages
    double rain_rate = ctx.gages.rainfall[0];  // User units (in/hr or mm/hr)

    // Convert to m/s
    double rain_m_per_s;
    if (static_cast<int>(ctx.options.flow_units) < 3) {
        // US customary (CFS/GPM/MGD): in/hr → m/s
        rain_m_per_s = rain_rate * 0.0254 / 3600.0;
    } else {
        // SI (CMS/LPS/MLD): mm/hr → m/s
        rain_m_per_s = rain_rate * 0.001 / 3600.0;
    }

    for (int i = 0; i < nt; ++i) {
        state_.rainfall[i] = rain_m_per_s;
    }
}

} // namespace openswmm::twoD
