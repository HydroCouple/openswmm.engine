// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file ArdEngine.cpp
 * @brief Solver-agnostic Eulerian ARD transport engine — phase E1 body.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "ArdEngine.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "../../../core/SimulationContext.hpp"
#include "../../../core/UnitConversion.hpp"
#include "../../../hydraulics/fv/NetworkMeshBuilder.hpp"
#include "../../fvkernels/SpeciesTransportKernels.hpp"
#include "../ReactionModule/ReactionArdBinding.hpp"

namespace openswmm::transport {

namespace fvk = openswmm::transport::fvkernels;
namespace k   = openswmm::fv::kernels;

namespace {
/// CFL number for the advection subcycling. Conservative: the kernels are
/// TVD at CFL ≤ 1; 0.9 leaves headroom for the projected-flux ramp.
constexpr double kCfl = 0.9;
/// Hard cap on substeps per routing step (pathological velocities).
constexpr int kMaxSubsteps = 512;
/// Below this store volume (ft3) a node holds no meaningful concentration —
/// used to decide when a store has emptied rather than merely shrunk.
constexpr double kMinStoreVol = 1.0e-9;

// --- E3 Fischer auto-computation guards (all internal ft units) -------------
/// g (ft/s²) for the shear velocity U* = √(g·Y·S).
constexpr double kGravity = 32.174;
/// Slope floor: a dead-flat conduit has S = 0 and the formula divides by
/// U* — the same reason legacy SWMM floors conduit slopes. Physics below
/// this floor is dominated by mixing the formula does not model anyway.
constexpr double kMinSlope = 1.0e-5;
/// Depth floor (ft): below this the conduit is effectively dry; v² in the
/// numerator vanishes faster than Y·U* in the denominator, so D → 0 is the
/// physical limit and also the numerically safe one.
constexpr double kMinFischerDepth = 0.01;
}  // namespace

// ===========================================================================
// init — mesh + state sizing
// ===========================================================================

bool ArdEngine::init(SimulationContext& ctx) {
    warnings_.clear();
    initialized_ = false;

    // E4/R6: the mesh state carries pollutants (rows 0..np-1, index-aligned
    // with the legacy pollutant index) plus the MSX species (rows np..) when
    // a reactions component is active — MSX species are TRANSPORTED under
    // this engine, retiring the R4b element-local limitation here. WALL
    // species have no transport semantics yet: fall back to LEGACY (whose
    // R4 binding runs them element-locally) with a precise warning.
    const int np = ctx.n_pollutants();
    int nm = 0;
    if (transport::ardReactionsActive(ctx)) {
        if (transport::ardHasWallSpecies(ctx)) {
            warnings_.push_back(
                "QUALITY_SOLVER EULERIAN_ARD: WALL species have no transport "
                "semantics under this engine yet — falling back to LEGACY, "
                "which runs them per element (R4).");
            return false;
        }
        nm = ctx.reactions.n_species();
    }
    const int ns = np + nm;
    if (ns <= 0) return false;

    const auto report = fv::buildNetworkMesh(ctx, ctx.options.fv, mesh_);
    for (const auto& w : report.warnings) warnings_.push_back(w);
    if (!report.errors.empty()) {
        for (const auto& e : report.errors)
            warnings_.push_back("EULERIAN_ARD mesh error: " + e);
        return false;
    }
    const int nc = mesh_.n_cells();
    const int nf = mesh_.n_faces();
    const int nn = static_cast<int>(mesh_.node_face_ptr.empty()
                                        ? 0
                                        : mesh_.node_face_ptr.size() - 1);
    if (nc <= 0) return false;

    state_.resize(nc, nn, ns);

    const auto unc = static_cast<std::size_t>(nc);
    const auto unf = static_cast<std::size_t>(nf);
    const auto uns = static_cast<std::size_t>(ns);

    face_q_.assign(unf, 0.0);
    f_mass_.assign(unf, 0.0);
    f_sstar_.assign(unf, 0.0);
    f_state_l_.assign(unf, k::FaceState{});
    f_state_r_.assign(unf, k::FaceState{});
    f_flux_.assign(unf, k::FaceFlux{});
    f_phi_l_.assign(uns * unf, 0.0);
    f_phi_r_.assign(uns * unf, 0.0);
    f_phi_flux_.assign(uns * unf, 0.0);
    cell_slope_.assign(unc, 0.0);
    lo_flux_.assign(unf, 0.0);
    anti_flux_.assign(unf, 0.0);
    td_.assign(unc, 0.0);
    anew_.assign(unc, 0.0);
    rplus_.assign(unc, 1.0);
    rminus_.assign(unc, 1.0);
    cell_u_.assign(unc, 0.0);
    cell_active_.assign(unc, char{1});
    active_faces_.resize(unf);
    for (int f = 0; f < nf; ++f) active_faces_[static_cast<std::size_t>(f)] = f;

    node_mass_.assign(static_cast<std::size_t>(nn) * uns, 0.0);
    node_vol_.assign(static_cast<std::size_t>(nn), 0.0);

    // Initial state: cell areas from link volume spread uniformly over the
    // conduit; pollutant rows (s < np) from the link's/node's initial
    // quality — the legacy arrays are np-STRIDED, not ns-strided (E4/R6
    // stride audit); MSX rows (s >= np) from the component's GLOBAL initial
    // values, matching R4's element-state seeding.
    const auto unp = static_cast<std::size_t>(np);
    const int n_links = ctx.n_links();
    for (int r = 0; r < static_cast<int>(mesh_.conduit_link.size()); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        const int link = mesh_.conduit_link[ur];
        if (link < 0 || link >= n_links) continue;
        const int  b   = mesh_.conduit_cell_begin[ur];
        const int  n   = mesh_.conduit_cell_count[ur];
        double length = 0.0;
        for (int i = 0; i < n; ++i)
            length += mesh_.cell_dx[static_cast<std::size_t>(b + i)];
        const double vol = ctx.links.volume[static_cast<std::size_t>(link)];
        const double a0  = (length > 0.0) ? vol / length : 0.0;
        for (int i = 0; i < n; ++i) {
            const auto uc = static_cast<std::size_t>(b + i);
            state_.cell_a[uc] = a0;
            for (int s = 0; s < np; ++s) {
                state_.cell_phi[static_cast<std::size_t>(s) * unc + uc] =
                    ctx.links.conc[static_cast<std::size_t>(link) * unp +
                                   static_cast<std::size_t>(s)];
            }
            for (int m = 0; m < nm; ++m) {
                state_.cell_phi[static_cast<std::size_t>(np + m) * unc + uc] =
                    ctx.reactions.init_global[static_cast<std::size_t>(m)];
            }
        }
    }
    for (int nd = 0; nd < nn && nd < ctx.n_nodes(); ++nd) {
        const auto und = static_cast<std::size_t>(nd);
        node_vol_[und] = ctx.nodes.volume[und];
        for (int s = 0; s < np; ++s)
            node_mass_[und * uns + static_cast<std::size_t>(s)] =
                ctx.nodes.conc[und * unp + static_cast<std::size_t>(s)] *
                node_vol_[und];
        for (int m = 0; m < nm; ++m)
            node_mass_[und * uns + static_cast<std::size_t>(np + m)] =
                ctx.reactions.init_global[static_cast<std::size_t>(m)] *
                node_vol_[und];
    }

    // E2: structures (pumps/orifices/weirs/outlets) transport as zero-volume
    // passthrough of the donor node store (plan §2.1, element coverage) — no
    // warning needed any more.

    // E3: dispersion configuration from the transport.ard component. Values
    // arrive in project display units (len²/s); convert to internal ft²/s
    // exactly the way Router::initFv converts FV_DISPERSION.
    disp_active_    = false;
    disp_mode_      = static_cast<int>(ArdDispersionMode::OFF);
    disp_global_ft_ = 0.0;
    conduit_disp_ft_.assign(mesh_.conduit_link.size(), -1.0);
    cell_disp_.assign(unc, 0.0);
    if (ctx.ard_config.configured && ctx.ard_config.any_dispersion()) {
        const double ucf_len = ucf::Ucf[ucf::LENGTH][ucf::getUnitSystem(
            static_cast<int>(ctx.options.flow_units))];
        disp_mode_      = static_cast<int>(ctx.ard_config.dispersion_mode);
        disp_global_ft_ =
            ctx.ard_config.dispersion_value / (ucf_len * ucf_len);
        // Overrides are keyed by LINK index; invert mesh conduit_link so
        // each mesh conduit row learns its override (if any).
        for (std::size_t i = 0; i < ctx.ard_config.conduit_disp_link.size();
             ++i) {
            const int link = ctx.ard_config.conduit_disp_link[i];
            const double d_ft =
                ctx.ard_config.conduit_disp_value[i] / (ucf_len * ucf_len);
            for (std::size_t r = 0; r < mesh_.conduit_link.size(); ++r)
                if (mesh_.conduit_link[r] == link) conduit_disp_ft_[r] = d_ft;
        }
        disp_active_ = true;
    }

    // E4: kdecay is applied by the reaction stage (exact exponential) — the
    // E1 "not yet applied" warning is retired.

    // E5a: boundary/source rows onto the mesh. The rows were resolved to
    // indices at open (resolveArdTransportRows); here they map to species
    // ROWS (np + msx) and, for sources, to mesh conduit rows + lengths.
    bc_node_.clear(); bc_srow_.clear(); bc_ts_.clear();
    bc_value_.clear(); bc_now_.clear();
    src_crow_.clear(); src_srow_.clear(); src_ts_.clear();
    src_value_.clear(); src_len_.clear(); src_now_.clear();
    if (nm > 0) {
        const auto& cfg = ctx.ard_config;
        for (std::size_t i = 0; i < cfg.bc_node.size(); ++i) {
            if (cfg.bc_msx[i] < 0 || cfg.bc_msx[i] >= nm) continue;
            if (cfg.bc_node[i] < 0 || cfg.bc_node[i] >= nn) continue;
            bc_node_.push_back(cfg.bc_node[i]);
            bc_srow_.push_back(np + cfg.bc_msx[i]);
            bc_value_.push_back(cfg.bc_value[i]);
            bc_ts_.push_back(cfg.bc_ts[i]);
        }
        bc_now_.assign(bc_node_.size(), 0.0);
        for (std::size_t i = 0; i < cfg.src_link.size(); ++i) {
            if (cfg.src_msx[i] < 0 || cfg.src_msx[i] >= nm) continue;
            // Find the mesh conduit row carrying this link.
            int crow = -1;
            for (std::size_t r2 = 0; r2 < mesh_.conduit_link.size(); ++r2)
                if (mesh_.conduit_link[r2] == cfg.src_link[i]) {
                    crow = static_cast<int>(r2);
                    break;
                }
            if (crow < 0) continue;
            double length = 0.0;
            const int b2 = mesh_.conduit_cell_begin[static_cast<std::size_t>(crow)];
            const int n2 = mesh_.conduit_cell_count[static_cast<std::size_t>(crow)];
            for (int i2 = 0; i2 < n2; ++i2)
                length += mesh_.cell_dx[static_cast<std::size_t>(b2 + i2)];
            src_crow_.push_back(crow);
            src_srow_.push_back(np + cfg.src_msx[i]);
            src_value_.push_back(cfg.src_value[i]);
            src_ts_.push_back(cfg.src_ts[i]);
            src_len_.push_back(std::max(length, 1.0e-6));
        }
        src_now_.assign(src_crow_.size(), 0.0);
    }

    initialized_ = true;
    return true;
}

// ===========================================================================
// updateTransportRows — per-routing-step BC/source evaluation (E5a)
// ===========================================================================

void ArdEngine::updateTransportRows(SimulationContext& ctx) {
    for (std::size_t i = 0; i < bc_node_.size(); ++i) {
        double c = bc_value_[i];
        const int ts = bc_ts_[i];
        if (ts >= 0 && ts < static_cast<int>(ctx.tables.count()))
            c = table_tseries_lookup_cursor(ctx.tables[ts],
                                            ctx.current_date);
        bc_now_[i] = std::max(0.0, c);
    }
    for (std::size_t i = 0; i < src_crow_.size(); ++i) {
        double r = src_value_[i];   // already internal conc·ft³/s
        const int ts = src_ts_[i];
        if (ts >= 0 && ts < static_cast<int>(ctx.tables.count()))
            r = table_tseries_lookup_cursor(ctx.tables[ts],
                                            ctx.current_date) /
                kLitersPerFt3;      // ts values are species mass/s
        src_now_[i] = std::max(0.0, r);
    }
}

// ===========================================================================
// projectHydraulics — continuity-consistent face fluxes (plan §3.2)
// ===========================================================================

void ArdEngine::projectHydraulics(SimulationContext& ctx, double dt) {
    const int nf = mesh_.n_faces();
    std::fill(face_q_.begin(), face_q_.end(), 0.0);
    std::vector<char> face_set(static_cast<std::size_t>(nf), char{0});

    for (int r = 0; r < static_cast<int>(mesh_.conduit_link.size()); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        const int link = mesh_.conduit_link[ur];
        if (link < 0) continue;
        const auto ul = static_cast<std::size_t>(link);
        const int b = mesh_.conduit_cell_begin[ur];
        const int n = mesh_.conduit_cell_count[ur];
        if (n <= 0) continue;

        // Link flow + volume change over this routing step. The ramp
        // F_i = Q + dV/dt·(1/2 − i/n), i = 0..n (cell-boundary index along
        // the conduit axis), gives every cell exactly dV/n of storage change
        // and centres the profile on the solver's link flow. Uniform-field
        // preservation and exact conservation both follow (E1 gates).
        const double q  = ctx.links.flow[ul];
        const double dv = (dt > 0.0)
                              ? (ctx.links.volume[ul] -
                                 ctx.links.old_volume[ul]) / dt
                              : 0.0;

        for (int i = 0; i <= n; ++i) {
            // The face between cell (b+i-1) and (b+i); ends map through the
            // cells' own face indices. Walk via the cell face pair arrays.
            // Side convention (per the FCT gather in the kernels): side == 0
            // means the CELL IS LEFT of that face — the face sits at the
            // cell's axis-POSITIVE (downstream) end; side == 1 means the face
            // is the cell's upstream face.
            int face = -1;
            if (i < n) {
                const auto uc = static_cast<std::size_t>(b + i);
                face = (mesh_.cell_side0[uc] == 1) ? mesh_.cell_face0[uc]
                                                   : mesh_.cell_face1[uc];
                // upstream face of cell b+i (between b+i-1 and b+i)
            } else {
                const auto uc = static_cast<std::size_t>(b + n - 1);
                face = (mesh_.cell_side0[uc] == 0) ? mesh_.cell_face0[uc]
                                                   : mesh_.cell_face1[uc];
                // downstream face of the last cell
            }
            if (face < 0 || face >= nf) continue;
            const auto uf = static_cast<std::size_t>(face);
            const double frac = (n > 0)
                                    ? (0.5 - static_cast<double>(i) /
                                                 static_cast<double>(n))
                                    : 0.0;
            const double fq = q + dv * frac;
            if (face_set[uf]) {
                // Splice face shared with the neighbouring conduit (virtual
                // junction): take the mean of the two conduits' end values.
                face_q_[uf] = 0.5 * (face_q_[uf] + fq);
            } else {
                face_q_[uf]  = fq;
                face_set[uf] = char{1};
            }
        }
    }

    // Publish into the kernel-facing face arrays. Sign-of-flux upwinding is
    // the degenerate contact-speed criterion of the projected path (plan
    // §2, Riemann-solver clarification): sstar carries the flux sign.
    for (int f = 0; f < nf; ++f) {
        const auto uf = static_cast<std::size_t>(f);
        const double fq = face_q_[uf];
        f_mass_[uf]  = fq;
        f_sstar_[uf] = fq;
        k::FaceFlux fl;
        fl.mass  = fq;
        fl.sstar = fq;
        f_flux_[uf] = fl;

        const int cl = mesh_.face_cl[uf];
        const int cr = mesh_.face_cr[uf];
        k::FaceState sl{}, sr{};
        if (cl >= 0) {
            const auto ucl = static_cast<std::size_t>(cl);
            sl.a = state_.cell_a[ucl];
            sl.q = fq;
        }
        if (cr >= 0) {
            const auto ucr = static_cast<std::size_t>(cr);
            sr.a = state_.cell_a[ucr];
            sr.q = fq;
        }
        f_state_l_[uf] = sl;
        f_state_r_[uf] = sr;
    }

    // Cell velocities for the QUICKEST Courant number.
    const int nc = mesh_.n_cells();
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        const double a = state_.cell_a[uc];
        cell_u_[uc] = (a > k::kDryArea)
                          ? 0.5 * (face_q_[static_cast<std::size_t>(
                                       mesh_.cell_face0[uc])] +
                                   face_q_[static_cast<std::size_t>(
                                       mesh_.cell_face1[uc])]) / a
                          : 0.0;
    }
}

// ===========================================================================
// updateDispersion — per-cell coefficients for this routing step (E3)
// ===========================================================================

void ArdEngine::updateDispersion(SimulationContext& ctx) {
    const auto mode = static_cast<ArdDispersionMode>(disp_mode_);
    for (int r = 0; r < static_cast<int>(mesh_.conduit_link.size()); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        double d = 0.0;
        if (conduit_disp_ft_[ur] >= 0.0) {
            d = conduit_disp_ft_[ur];        // user override always wins
        } else if (mode == ArdDispersionMode::VALUE) {
            d = disp_global_ft_;
        } else if (mode == ArdDispersionMode::FISCHER) {
            // Fischer et al. (1979): D = 0.011 v²B²/(Y·U*), U* = √(g·Y·S) —
            // the CSH §4.2 coefficient model without the numerical-dispersion
            // correction (plan §2). Link-mean quantities, internal ft units:
            //   A = V/L, v = |Q|/A, Y = midpoint depth, B = A/Y (mean width),
            //   S = |conduit slope| floored.
            // A dry or stagnant conduit gets D = 0 (the physical limit).
            // Note D is unbounded above as Y → floor with v finite; that is
            // safe here because the implicit solve is unconditionally stable
            // and monotone — the worst case is over-mixing toward uniform,
            // never an overshoot.
            const int link = mesh_.conduit_link[ur];
            const int cr2  = (link >= 0)
                                 ? ctx.link_subtypes.conduit_row(link)
                                 : -1;
            if (link >= 0 && cr2 >= 0) {
                const auto ul   = static_cast<std::size_t>(link);
                const auto ucr  = static_cast<std::size_t>(cr2);
                const double len =
                    ctx.link_subtypes.conduits.length[ucr];
                const double vol = ctx.links.volume[ul];
                const double y   = ctx.links.depth[ul];
                const double a   =
                    (len > 0.0) ? vol / len : 0.0;
                if (a > k::kDryArea && y > kMinFischerDepth) {
                    const double vel = std::fabs(ctx.links.flow[ul]) / a;
                    if (vel > 0.0) {
                        const double b_w = a / y;
                        const double s = std::max(
                            std::fabs(ctx.link_subtypes.conduits.slope[ucr]),
                            kMinSlope);
                        const double ustar = std::sqrt(kGravity * y * s);
                        d = 0.011 * vel * vel * b_w * b_w / (y * ustar);
                    }
                }
            }
        }
        const int b = mesh_.conduit_cell_begin[ur];
        const int n = mesh_.conduit_cell_count[ur];
        for (int i = 0; i < n; ++i)
            cell_disp_[static_cast<std::size_t>(b + i)] = d;
    }
}

// ===========================================================================
// substep — advection + node mixing over dt_sub
// ===========================================================================

void ArdEngine::substep(SimulationContext& ctx, double dt_sub,
                        double load_frac) {
    const int ns = state_.n_species;
    const int nc = mesh_.n_cells();
    const int nf = mesh_.n_faces();
    const auto uns = static_cast<std::size_t>(ns);
    const auto unc = static_cast<std::size_t>(nc);
    const auto unf = static_cast<std::size_t>(nf);

    // 1. Interior reconstruction + FCT through the shared kernels.
    fvk::SpeciesKernelView v;
    v.mesh          = &mesh_;
    v.state         = &state_;
    v.scalar_scheme = ctx.options.fv.scalar_scheme;
    v.limiter       = ctx.options.fv.limiter;
    v.dispersion    = 0.0;   // scalar path unused; per-cell array below (E3)
    v.cell_dispersion = disp_active_ ? &cell_disp_ : nullptr;
    v.hllc          = true;  // sstar = flux sign (projected path)
    v.f_mass        = &f_mass_;
    v.f_sstar       = &f_sstar_;
    v.f_state_l     = &f_state_l_;
    v.f_state_r     = &f_state_r_;
    v.f_flux        = &f_flux_;
    v.cell_u        = &cell_u_;
    v.cell_active   = &cell_active_;
    v.active_faces  = &active_faces_;
    v.f_phi_l       = &f_phi_l_;
    v.f_phi_r       = &f_phi_r_;
    v.f_phi_flux    = &f_phi_flux_;
    v.cell_slope    = &cell_slope_;
    v.lo_flux       = &lo_flux_;
    v.anti_flux     = &anti_flux_;
    v.td            = &td_;
    v.anew          = &anew_;
    v.rplus         = &rplus_;
    v.rminus        = &rminus_;
    fvk::reconstructScalars(v, dt_sub);

    // 2. Node boundary faces: replace the kernels' zero-gradient ghost with
    //    the node store as the inflow donor (fixes the FV plan's "no node
    //    concentration" gap for this engine). Outflow (cell → node) keeps
    //    the cell donor value the kernels already assembled.
    const int nn = static_cast<int>(node_mass_.size() / std::max<std::size_t>(uns, 1));
    for (int nd = 0; nd < nn; ++nd) {
        const auto und = static_cast<std::size_t>(nd);
        const int b = mesh_.node_face_ptr[und];
        const int e = mesh_.node_face_ptr[und + 1];
        for (int p = b; p < e; ++p) {
            const auto uf = static_cast<std::size_t>(
                mesh_.node_face_idx[static_cast<std::size_t>(p)]);
            const double sign = mesh_.node_face_sign[static_cast<std::size_t>(p)];
            const double fq = face_q_[uf];
            // Flux INTO the node when sign*fq > 0; into the CELL otherwise.
            if (sign * fq < 0.0) {
                // An empty store has no concentration to donate. Dividing the
                // residual mass by a 1e-12 floor instead handed the receiving
                // cell an astronomical donor value, and on a storm deck under
                // FV that injection ran away (outfall load ~1e25). Below the
                // store threshold the donor contributes nothing, which is both
                // stable and what "the node holds no water" means.
                const double vol = node_vol_[und];
                for (int s = 0; s < ns; ++s) {
                    const double cnode =
                        (vol > kMinStoreVol)
                            ? node_mass_[und * uns +
                                         static_cast<std::size_t>(s)] / vol
                            : 0.0;
                    f_phi_flux_[static_cast<std::size_t>(s) * unf + uf] =
                        fq * cnode;
                }
            }
        }
    }

    // 3. Cell update: species mass and area from the SAME fluxes.
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        const int faces[2]    = {mesh_.cell_face0[uc], mesh_.cell_face1[uc]};
        const int8_t sides[2] = {mesh_.cell_side0[uc], mesh_.cell_side1[uc]};
        const double inv_dx = 1.0 / mesh_.cell_dx[uc];
        double dA = 0.0;
        for (int e2 = 0; e2 < 2; ++e2) {
            const auto uf = static_cast<std::size_t>(faces[e2]);
            const double sg = (sides[e2] == 0) ? -1.0 : 1.0;
            dA += sg * f_mass_[uf];
        }
        const double a_old = state_.cell_a[uc];
        const double a_new = std::max(0.0, a_old + dt_sub * dA * inv_dx);
        for (int s = 0; s < ns; ++s) {
            const auto sb = static_cast<std::size_t>(s);
            double dm = 0.0;
            for (int e2 = 0; e2 < 2; ++e2) {
                const auto uf = static_cast<std::size_t>(faces[e2]);
                const double sg = (sides[e2] == 0) ? -1.0 : 1.0;
                dm += sg * f_phi_flux_[sb * unf + uf];
            }
            const double m = a_old * state_.cell_phi[sb * unc + uc] +
                             dt_sub * dm * inv_dx;
            state_.cell_phi[sb * unc + uc] =
                (a_new > k::kDryArea) ? std::max(0.0, m) / a_new
                                      : state_.cell_phi[sb * unc + uc];
        }
        state_.cell_a[uc] = a_new;
    }

    // 4. Node stores: face exchange + the prorated external loads
    //    (qual_mass_in / qual_vol_in were assembled for the WHOLE routing
    //    step by the QualitySolver loaders; load_frac = dt_sub / dt_step).
    for (int nd = 0; nd < nn && nd < ctx.n_nodes(); ++nd) {
        const auto und = static_cast<std::size_t>(nd);
        const int b = mesh_.node_face_ptr[und];
        const int e = mesh_.node_face_ptr[und + 1];
        double dvol = 0.0;
        for (int p = b; p < e; ++p) {
            const auto uf = static_cast<std::size_t>(
                mesh_.node_face_idx[static_cast<std::size_t>(p)]);
            const double sign = mesh_.node_face_sign[static_cast<std::size_t>(p)];
            const double fq = face_q_[uf];
            dvol += sign * fq;
            for (int s = 0; s < ns; ++s) {
                node_mass_[und * uns + static_cast<std::size_t>(s)] +=
                    dt_sub * sign *
                    f_phi_flux_[static_cast<std::size_t>(s) * unf + uf];
            }
        }
        node_vol_[und] = std::max(0.0, node_vol_[und] + dt_sub * dvol);
        // External loads (washoff, DWF, RDII, GW, iface, direct inflows).
        // NOTE the asymmetry, which is not a typo: qual_vol_in is an AMOUNT
        // already integrated over the routing step (the loaders add q*dt), so
        // it is prorated by load_frac; qual_mass_in is a RATE (mass/sec — see
        // mixAtNodes, which multiplies it by dt), so it integrates over dt_sub.
        // Prorating the rate instead of integrating it made every external
        // load land a factor of dt_step too small.
        //
        // E4/R6 stride audit: qual_mass_in is a POLLUTANT array
        // ([node * np + p]) — only the pollutant rows receive external
        // loads. MSX species have no loading pathway until [TRANSPORT_
        // SOURCES]/[TRANSPORT_BOUNDARIES] land (E5): inflow water carries
        // ZERO MSX concentration, so sustained inflow dilutes MSX stores —
        // the documented default, and the transport observable the R6 gate
        // rides on.
        node_vol_[und] += load_frac * ctx.nodes.qual_vol_in[und];
        const int np_l = ctx.n_pollutants();
        const auto unp_l = static_cast<std::size_t>(np_l);
        for (int s = 0; s < np_l; ++s) {
            node_mass_[und * uns + static_cast<std::size_t>(s)] += std::max(
                0.0, dt_sub *
                         ctx.nodes.qual_mass_in[und * unp_l +
                                                static_cast<std::size_t>(s)]);
            // Persistent user quality mass flux is NOT added here: it is folded
            // into qual_mass_in by QualitySolver::addExtInflowLoads(), the same
            // loader stage legacy uses, so the line above already carries it.
            // Adding it a second time here would double the forced mass.
        }
        // The non-negativity floor is a property of a STORE, not of a
        // pollutant, so it runs over every row — including the MSX rows the
        // load loop above deliberately skips. Narrowing this clamp along
        // with the load stride left MSX store masses unfloored: a node that
        // repeatedly empties (the junction just upstream of an outfall)
        // accumulated a large oscillating NEGATIVE mass, which the node
        // boundary-face override then donated into the first cell of the
        // adjoining conduit. Symptom was a 0.67 mg/L divergence confined to
        // the outfall-adjacent conduit's MSX rows while every pollutant row
        // stayed bit-identical.
        for (int s = 0; s < ns; ++s)
            node_mass_[und * uns + static_cast<std::size_t>(s)] =
                std::max(0.0, node_mass_[und * uns +
                                         static_cast<std::size_t>(s)]);
    }

    // 5. Structures (E2): pumps/orifices/weirs/outlets are zero-volume
    //    passthrough elements (plan §2.1 element coverage; same table as
    //    LARD §2). Water the hydraulic solver moves through a structure
    //    carries the DONOR node store's concentration; both stores update
    //    volume and mass symmetrically. Donor below the store-empty
    //    threshold moves water but no mass (consistent with the boundary
    //    donor guard above). Mass is capped at what the donor holds so a
    //    large q*dt_sub cannot go negative.
    const int n_struct = static_cast<int>(mesh_.struct_link.size());
    for (int i = 0; i < n_struct; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const int link = mesh_.struct_link[ui];
        if (link < 0 || link >= ctx.n_links()) continue;
        const double q = ctx.links.flow[static_cast<std::size_t>(link)];
        if (q == 0.0) continue;
        const int donor_n    = (q >= 0.0) ? mesh_.struct_n1[ui] : mesh_.struct_n2[ui];
        const int receiver_n = (q >= 0.0) ? mesh_.struct_n2[ui] : mesh_.struct_n1[ui];
        if (donor_n < 0 || receiver_n < 0 || donor_n >= nn || receiver_n >= nn)
            continue;
        const auto ud = static_cast<std::size_t>(donor_n);
        const auto ur = static_cast<std::size_t>(receiver_n);
        const double dv = std::fabs(q) * dt_sub;
        const double donor_vol = node_vol_[ud];
        if (donor_vol > kMinStoreVol) {
            for (int s = 0; s < ns; ++s) {
                const auto sb = static_cast<std::size_t>(s);
                const double c_donor = node_mass_[ud * uns + sb] / donor_vol;
                const double dm = std::min(dv * c_donor,
                                           node_mass_[ud * uns + sb]);
                node_mass_[ud * uns + sb] -= dm;
                node_mass_[ur * uns + sb] += dm;
            }
        }
        node_vol_[ud] = std::max(0.0, node_vol_[ud] - dv);
        node_vol_[ur] += dv;
    }

    // 5b. E5a transport boundaries: the node's external inflow water (the
    //     same qual_vol_in the pollutant loaders integrated) carries each
    //     boundary MSX species at its current concentration — internal
    //     store mass is conc·ft³, so vol·conc needs no conversion. Rows on
    //     dry-inflow nodes add nothing (vol_ext = 0).
    for (std::size_t i = 0; i < bc_node_.size(); ++i) {
        const auto und = static_cast<std::size_t>(bc_node_[i]);
        if (static_cast<int>(und) >= nn) continue;
        const double vol_ext = load_frac * ctx.nodes.qual_vol_in[und];
        if (vol_ext > 0.0 && bc_now_[i] > 0.0)
            node_mass_[und * uns + static_cast<std::size_t>(bc_srow_[i])] +=
                vol_ext * bc_now_[i];
    }

    // 5c. E5a distributed sources: internal rate r (conc·ft³/s) spread over
    //     the conduit's cells ∝ dx ⇒ Δconc per cell = r·dt/(L·a) — dx
    //     cancels. Dry cells are skipped and their share is NOT delivered
    //     (a source into a dry conduit has no water to dissolve into);
    //     documented, and the mass-balance ledger (E5b) will book the
    //     undelivered remainder explicitly.
    for (std::size_t i = 0; i < src_crow_.size(); ++i) {
        const double r = src_now_[i];
        if (r <= 0.0) continue;
        const auto ucrow = static_cast<std::size_t>(src_crow_[i]);
        const int b2 = mesh_.conduit_cell_begin[ucrow];
        const int n2 = mesh_.conduit_cell_count[ucrow];
        const auto sb = static_cast<std::size_t>(src_srow_[i]);
        for (int i2 = 0; i2 < n2; ++i2) {
            const auto uc = static_cast<std::size_t>(b2 + i2);
            const double a = state_.cell_a[uc];
            if (a <= k::kDryArea) continue;
            state_.cell_phi[sb * unc + uc] +=
                dt_sub * r / (src_len_[i] * a);
        }
    }

    // 6. Dispersion (E3): implicit per-chain Thomas solve over the updated
    //    cells — sequential (Lie) operator split, advection → dispersion
    //    (plan §2). One full step of each, so the splitting error is O(dt),
    //    not the O(dt²) a symmetric Strang split would give; the substep is
    //    already Courant-limited by advection, and the dispersion half is
    //    unconditionally stable, so the first-order split is the cheap
    //    choice rather than an accuracy claim. Symmetrizing it is an E5
    //    option, not a defect. The kernel
    //    early-outs when v.cell_dispersion is null (disp_active_ false), so
    //    the pre-E3 substep is bit-identical. Node stores do not disperse:
    //    the solve spans conduit chains (including splice virtual
    //    junctions), matching the FV solver's D-FV1 behavior; junction
    //    mixing remains the CSTR exchange above.
    fvk::dispersionSolve(v, dt_sub);
}

// ===========================================================================
// step
// ===========================================================================

void ArdEngine::step(SimulationContext& ctx, double dt) {
    if (!initialized_ || dt <= 0.0) return;

    // conc_old bookkeeping matches the legacy engine's convention.
    ctx.links.conc_old = ctx.links.conc;
    ctx.nodes.conc_old = ctx.nodes.conc;

    projectHydraulics(ctx, dt);
    if (disp_active_) updateDispersion(ctx);
    if (!bc_node_.empty() || !src_crow_.empty()) updateTransportRows(ctx);

    // Node volumes resync to the solver's each routing step (the store keeps
    // its own between substeps only).
    const int ns  = state_.n_species;
    const int nn  = std::min(ctx.n_nodes(),
                             static_cast<int>(node_vol_.size()));
    const auto uns = static_cast<std::size_t>(ns);
    for (int nd = 0; nd < nn; ++nd) {
        const auto und = static_cast<std::size_t>(nd);
        const double v_old = node_vol_[und];
        const double v_new = std::max(ctx.nodes.volume[und], 0.0);

        // Water that leaves the SYSTEM — outfall discharge, flooding — rides no
        // mesh face, so the store never saw it go. Preserving mass across the
        // resync (as this did) left every boundary node accumulating mass it
        // could not shed while its volume snapped back toward zero, and
        // conc = mass/vol diverged: an outfall manufactured mass quadratically
        // in time. A well-mixed store discharges at its OWN concentration, so
        // the mass follows the water down. Scale DOWN only: a store whose
        // volume the solver reports as larger has gained water the store did
        // not track, and scaling up would create mass from nothing.
        if (v_old > kMinStoreVol) {
            const double ratio = v_new / v_old;
            if (ratio < 1.0)
                for (int s = 0; s < ns; ++s)
                    node_mass_[und * uns + static_cast<std::size_t>(s)] *= ratio;
        } else if (v_new <= kMinStoreVol) {
            for (int s = 0; s < ns; ++s)
                node_mass_[und * uns + static_cast<std::size_t>(s)] = 0.0;
        }
        node_vol_[und] = v_new;
    }

    // CFL subcycling on the projected velocities.
    double dt_cfl = dt;
    const int nc = mesh_.n_cells();
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        const double u = std::fabs(cell_u_[uc]);
        if (u > 1.0e-12)
            dt_cfl = std::min(dt_cfl, kCfl * mesh_.cell_dx[uc] / u);
    }
    int nsub = static_cast<int>(std::ceil(dt / std::max(dt_cfl, 1.0e-12)));
    if (nsub > kMaxSubsteps) {
        // E2 (validation note 5.3): the clamp must be LOUD — a silently
        // clamped run violates CFL and the max principle can leak. Warn once
        // per run with the achieved ratio so the user can shorten
        // ROUTING_STEP or coarsen FV_CELL_LENGTH.
        if (!warned_cfl_clamp_) {
            warned_cfl_clamp_ = true;
            ctx.warnings.push_back(
                "QUALITY_SOLVER EULERIAN_ARD: transport subcycling clamped at " +
                std::to_string(kMaxSubsteps) + " substeps (needed " +
                std::to_string(nsub) +
                ") — effective CFL exceeds the stability target; shorten "
                "ROUTING_STEP or increase FV_CELL_LENGTH.");
        }
        nsub = kMaxSubsteps;
    }
    const double dt_sub = dt / static_cast<double>(nsub);
    const double load_frac = 1.0 / static_cast<double>(nsub);

    for (int i = 0; i < nsub; ++i) substep(ctx, dt_sub, load_frac);

    // E4: reaction stage over the full routing step, Lie-split after the
    // advection–dispersion subcycle (roadmap lesson 13 — first-order
    // splitting, documented; the integrator substeps adaptively inside):
    // exact-exponential kdecay on pollutant rows + MSX integration per cell
    // (pipe scope) and per node store (tank scope).
    transport::reactArdStage(
        ctx, dt, state_.cell_phi.data(), state_.cell_a.data(),
        mesh_.n_cells(), node_mass_.data(), node_vol_.data(),
        static_cast<int>(node_vol_.size()), ctx.n_pollutants(),
        state_.n_species, kMinStoreVol);

    publish(ctx);
}

// ===========================================================================
// publish — cell state → legacy link/node arrays
// ===========================================================================

void ArdEngine::publish(SimulationContext& ctx) {
    const int ns = state_.n_species;
    const int np = ctx.n_pollutants();
    const int nm = ns - np;   // MSX rows (0 when no reactions component)
    const auto uns = static_cast<std::size_t>(ns);
    const auto unp = static_cast<std::size_t>(np);
    const auto unm = static_cast<std::size_t>(nm > 0 ? nm : 0);
    const auto unc = static_cast<std::size_t>(mesh_.n_cells());

    // E4/R6: MSX rows publish into the R4 element-state arrays
    // ([element * nm + m]) so reporting and the C API read one place under
    // either engine. Sized here (NOT via the legacy binding's ensure, whose
    // R4b warning does not apply — species ARE transported under ARD).
    auto& rx = ctx.reactions;
    if (nm > 0) {
        const auto want_l = static_cast<std::size_t>(ctx.n_links()) * unm;
        const auto want_n = static_cast<std::size_t>(ctx.n_nodes()) * unm;
        if (rx.msx_link_conc.size() != want_l)
            rx.msx_link_conc.assign(want_l, 0.0);
        if (rx.msx_node_conc.size() != want_n)
            rx.msx_node_conc.assign(want_n, 0.0);
    }

    for (int r = 0; r < static_cast<int>(mesh_.conduit_link.size()); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        const int link = mesh_.conduit_link[ur];
        if (link < 0 || link >= ctx.n_links()) continue;
        const int b = mesh_.conduit_cell_begin[ur];
        const int n = mesh_.conduit_cell_count[ur];
        for (int s = 0; s < ns; ++s) {
            const auto sb = static_cast<std::size_t>(s);
            double m = 0.0, vol = 0.0;
            for (int i = 0; i < n; ++i) {
                const auto uc = static_cast<std::size_t>(b + i);
                const double dv = state_.cell_a[uc] * mesh_.cell_dx[uc];
                m   += dv * state_.cell_phi[sb * unc + uc];
                vol += dv;
            }
            const double conc = (vol > 1.0e-12) ? m / vol : 0.0;
            if (s < np)
                ctx.links.conc[static_cast<std::size_t>(link) * unp + sb] =
                    conc;
            else
                rx.msx_link_conc[static_cast<std::size_t>(link) * unm +
                                 static_cast<std::size_t>(s - np)] = conc;
        }
    }

    const int nn = std::min(ctx.n_nodes(),
                            static_cast<int>(node_vol_.size()));
    for (int nd = 0; nd < nn; ++nd) {
        const auto und = static_cast<std::size_t>(nd);
        const double vol = node_vol_[und];
        for (int s = 0; s < ns; ++s) {
            const double conc =
                (vol > 1.0e-12)
                    ? node_mass_[und * uns + static_cast<std::size_t>(s)] / vol
                    : 0.0;
            if (s < np)
                ctx.nodes.conc[und * unp + static_cast<std::size_t>(s)] = conc;
            else
                rx.msx_node_conc[und * unm +
                                 static_cast<std::size_t>(s - np)] = conc;
        }
    }

    // Structure links (E2) report the donor node's published concentration —
    // the zero-volume passthrough convention (matches legacy, which sets a
    // structure's quality from its upstream node). E4/R6: MSX rows too.
    const int n_struct = static_cast<int>(mesh_.struct_link.size());
    for (int i = 0; i < n_struct; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const int link = mesh_.struct_link[ui];
        if (link < 0 || link >= ctx.n_links()) continue;
        const double q = ctx.links.flow[static_cast<std::size_t>(link)];
        const int donor_n = (q >= 0.0) ? mesh_.struct_n1[ui] : mesh_.struct_n2[ui];
        if (donor_n < 0 || donor_n >= nn) continue;
        const auto ud = static_cast<std::size_t>(donor_n);
        for (int s = 0; s < np; ++s)
            ctx.links.conc[static_cast<std::size_t>(link) * unp +
                           static_cast<std::size_t>(s)] =
                ctx.nodes.conc[ud * unp + static_cast<std::size_t>(s)];
        for (int m2 = 0; m2 < nm; ++m2)
            rx.msx_link_conc[static_cast<std::size_t>(link) * unm +
                             static_cast<std::size_t>(m2)] =
                rx.msx_node_conc[ud * unm + static_cast<std::size_t>(m2)];
    }
}

// ===========================================================================
// totalMass — conservation ledger for the unit gates
// ===========================================================================

std::vector<double> ArdEngine::totalMass(const SimulationContext& ctx) const {
    (void)ctx;
    const int ns = state_.n_species;
    const auto uns = static_cast<std::size_t>(ns);
    const auto unc = static_cast<std::size_t>(mesh_.n_cells());
    std::vector<double> total(uns, 0.0);
    for (int s = 0; s < ns; ++s) {
        const auto sb = static_cast<std::size_t>(s);
        double m = 0.0;
        for (std::size_t c = 0; c < unc; ++c)
            m += state_.cell_a[c] * mesh_.cell_dx[c] * state_.cell_phi[sb * unc + c];
        for (std::size_t nd = 0; nd < node_vol_.size(); ++nd)
            m += node_mass_[nd * uns + sb];
        total[sb] = m;
    }
    return total;
}

}  // namespace openswmm::transport
