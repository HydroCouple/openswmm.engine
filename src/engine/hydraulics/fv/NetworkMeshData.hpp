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
 * @file NetworkMeshData.hpp
 * @brief SoA storage for the 1D finite-volume network mesh and its state.
 *
 * @details The FV counterpart of 2d/data/MeshData.hpp. Three index spaces:
 *
 *            cells  [0, n_cells)  — one control volume, U = [A, Q]
 *            faces  [0, n_faces)  — one interface, flux positive LEFT → RIGHT
 *            nodes  [0, n_nodes)  — the model's SWMM nodes, zero-D volumes
 *
 *          The mesh is INTERNAL numerical discretization and carries no named
 *          objects: conduits are cut into cell chains, virtual junctions become
 *          plain interior faces splicing two chains, and regular nodes become
 *          coupled zero-D volumes reached through boundary faces (plan §3.4).
 *
 *          Static after init — deliberately: fixed-size arrays and a fixed face
 *          graph are what keep the device-resident Kokkos design free of view
 *          rebuilds mid-run. Adaptivity is in time (LTS), not space (plan §3.2).
 *
 * @see plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md §3.2, §4
 * @ingroup engine_fv
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_NETWORK_MESH_DATA_HPP
#define OPENSWMM_ENGINE_FV_NETWORK_MESH_DATA_HPP

#include <cstdint>
#include <vector>

#include "../XSectBatch.hpp"
#include "../HydClosureKernels.hpp"
#include "../XSectKernels.hpp"

namespace openswmm::fv {

/// Samples in the per-geometry first-moment (I₁) table over [0, y_full].
/// I₁ is the antiderivative of A(h); above the crown the closure is exactly
/// linear in h so the table is extended analytically rather than sampled
/// (see FvKernels::i1OfDepth), which is what keeps deep surcharge accurate
/// with a small table.
inline constexpr int kI1Samples = 129;

/// Samples in a STORAGE node's flattened depth→volume table.
inline constexpr int kNodeVolSamples = 129;

/**
 * @brief Cross-section closure for one conduit's cell chain.
 *
 * One continuous geometry valid from dry bed to full pressurization: the
 * Preissmann slot is folded into A(h)/T(h)/R(h) so every cell evaluates the
 * same flux function and "transition" is just a depth crossing the crown
 * (plan §3.3). `t_slot` is derived from FV_SLOT_CELERITY, and the slot mouth is
 * tapered over [y_crown, y_full] so dA/dh stays Lipschitz through the crown —
 * a discontinuous dA/dh would produce spurious reflections and corrupt the
 * Riemann solver's wave-speed estimates.
 */
struct FvGeometry {
    XSectParams xs{};              ///< section parameters (owns transect table ptrs)

    /// Where the section's own geometry is evaluated. A pointer, not a copy:
    /// the evaluator carries the shared geometry tables, and which memory space
    /// those live in is exactly what differs between the host solver and the
    /// device backend. The mesh builder binds this to xsect::hostEval(); a
    /// device backend rebinds it to its own device-resident pair, and the same
    /// kernel bodies then run unchanged on both (plan §5.1).
    const xsect::XsectEval* eval = nullptr;

    double y_full = 0.0;           ///< full depth (ft)
    double a_full = 0.0;           ///< area when full (ft²)
    double w_max  = 0.0;           ///< width at widest point (ft)
    double r_full = 0.0;           ///< hydraulic radius when full (ft)

    /// `barrels` as a factor on AREA and TOP WIDTH (see `barrels` below). Kept
    /// beside the fields every area evaluation already loads.
    double barrel_scale = 1.0;

    /// Depth at which the slot begins to open. Closed shapes: SLOT_CROWN_CUTOFF
    /// · y_full (Sjöberg-style, engages just below the crown). Open shapes:
    /// y_full, with no taper — the section simply continues with vertical walls.
    double y_crown = 0.0;

    /// Top width of the slot above the crown, g·A_full/c_slot². Open shapes
    /// carry w_max here so the vertical-wall extension is one code path.
    double t_slot = 0.0;

    /// A(y_full) INCLUDING the slot area accumulated over the tapered mouth.
    /// Cached because every above-crown evaluation and the analytic I₁
    /// extension start from it.
    double a_crown = 0.0;

    /// I₁(y_full) — the table's last entry, cached for the same reason.
    double i1_crown = 0.0;

    uint8_t is_open = 0;           ///< open section: no crown, no slot taper

    // -- Friction and losses (already lengthening-adjusted, see Router::init) --

    double roughness    = 0.01;    ///< Manning n
    double rough_factor = 0.0;     ///< g·(n/PHI)² — friction denominator factor
    double loss_inlet   = 0.0;     ///< entrance loss coefficient K
    double loss_outlet  = 0.0;     ///< exit loss coefficient K

    int    barrels = 1;            ///< parallel identical barrels

    /// [XSECTIONS] culvert code (0 = not a culvert), and the conduit slope the
    /// HEC-5 inlet-control equations need. Cold: read only at the conduit's
    /// upstream boundary face, and only when the code is set.
    int    culvert_code = 0;
    double slope        = 0.0;

    /// The type code's inlet-control curve, RESOLVED at mesh build. Keeping the
    /// resolved coefficients rather than the code is what lets the solver
    /// evaluate the closure with no engine dependency — the table lookup is
    /// host work, the curve is all the kernel needs.
    hydkernels::CulvertCurve culvert_curve{};
    uint8_t culvert_mitered = 0;

    /// First moment I₁(h) = ∫₀ʰ A(η)dη sampled uniformly on h ∈ [0, y_full],
    /// followed by the companion A(h) samples on the same grid — 2·kI1Samples
    /// entries, `[0, kI1Samples)` = I₁ and `[kI1Samples, 2·kI1Samples)` = A.
    /// One buffer because both are read together on every evaluation.
    ///
    /// Built once at init by composite integration of the same A(h) the solver
    /// evaluates. Quadrature error does not threaten well-balancedness — that
    /// needs only a single-valued I₁(h) — but it does set the accuracy of the
    /// pressure term, hence the fine sub-sampling in buildI1Table.
    ///
    /// A fixed inline array rather than a vector: the whole struct is copied
    /// into device memory by the accelerated backend, and an owning container
    /// cannot cross that boundary. At 129 samples this is 2 kB per DISTINCT
    /// cross-section — a few hundred at most in a real model.
    double i1_tbl[2 * kI1Samples] = {};

    /// The INVERSE of the area column: depth sampled uniformly in AREA over
    /// [0, a_crown], `h_tbl[j]` being the exact root of A(h) = j·a_crown/(n−1).
    ///
    /// The forward table is uniform in depth, which is the wrong grid to invert
    /// on. Bracketing a query area in it costs a binary search — seven
    /// dependent loads with unpredictable branches — and near the crown, where
    /// A is nearly flat in h, one depth panel spans a wide range of areas, so
    /// the bracket it yields is loose and the root-find needs several
    /// evaluations of the closure. Profiling put `depthOfArea` and the area
    /// lookups it drives at 87 % of solver time on a Δx = 20 ft run.
    ///
    /// Sampling uniformly in area instead makes the panel a single divide, and
    /// makes the residuals at its two ends known WITHOUT evaluating the
    /// closure — they are the sample areas themselves. Built at init from the
    /// bracketed inverse, so it costs nothing at run time.
    double h_tbl[kI1Samples] = {};

};

/**
 * @brief SoA mesh geometry and topology for the FV network solver.
 */
struct NetworkMeshData {

    // -----------------------------------------------------------------------
    // Cells — indexed by cell index [0, n_cells)
    // -----------------------------------------------------------------------

    std::vector<int>    cell_geom;    ///< index into `geom`
    std::vector<int>    cell_conduit; ///< ConduitData row (for reporting)
    std::vector<double> cell_dx;      ///< cell length (ft)
    std::vector<double> cell_zb;      ///< bed elevation at cell centre (ft)

    /// dz/dx along the cell's OWN axis. The bed is linear within a conduit, so
    /// this is exact rather than reconstructed — which matters, because the
    /// second-order scheme reconstructs the free surface and the bed
    /// SEPARATELY and their difference is the depth it hands the Riemann
    /// solver. A limited bed slope would make lake-at-rest only approximately
    /// well balanced.
    std::vector<double> cell_dzdx;

    /// The two faces bounding each cell, with the side the cell occupies on
    /// each (0 = left, 1 = right). A 1D cell has exactly two, so this replaces
    /// a CSR: the cell update becomes a fixed-width GATHER with no atomics and
    /// no scatter races, which is what makes the update deterministic and
    /// identical across backends and thread counts.
    std::vector<int>    cell_face0;
    std::vector<int>    cell_face1;
    std::vector<int8_t> cell_side0;
    std::vector<int8_t> cell_side1;

    // -----------------------------------------------------------------------
    // Faces — indexed by face index [0, n_faces). Flux is positive LEFT → RIGHT.
    // -----------------------------------------------------------------------
    // A face with cell_l == -1 has a NODE on its left (a conduit's upstream
    // boundary); cell_r == -1 puts the node on its right (downstream boundary).
    // Interior faces (both cells ≥ 0) include virtual junctions, which are
    // nothing but a splice of two conduits' chains — mass and momentum flux
    // continuity are then properties of the scheme, not a special treatment.

    std::vector<int>    face_cl;      ///< left cell  (-1 ⇒ node on the left)
    std::vector<int>    face_cr;      ///< right cell (-1 ⇒ node on the right)
    std::vector<int>    face_node;    ///< coupled node (-1 for interior faces)

    /// Flap-gate mask on a boundary face: bit 0 blocks a POSITIVE mass flux,
    /// bit 1 blocks a NEGATIVE one, 0 leaves the face open. A gate is a check
    /// valve, so a blocked face behaves as a wall only while the flux would run
    /// the wrong way. Both bits can be set at once — a gated conduit pointing
    /// out of a gated outfall is sealed in both directions, exactly as DW's two
    /// independent checks leave it.
    std::vector<uint8_t> face_gate;

    /// Conduit row whose CULVERT inlet this face is, or -1. Set only on the
    /// upstream boundary face of a conduit carrying a culvert code, which is
    /// where HEC-5 inlet control physically acts.
    std::vector<int> face_culvert;
    std::vector<double> face_zb;      ///< bed elevation at the interface (ft)
    std::vector<double> face_dx;      ///< centre-to-centre distance (ft)

    /// Orientation of each incident cell's own axis relative to the face's
    /// LEFT→RIGHT positive direction. +1 is the normal case (the face sits at
    /// cell L's downstream end and cell R's upstream end); −1 appears when a
    /// virtual junction splices two conduits that both point INTO it (or both
    /// out of it), which the [VIRTUAL_JUNCTIONS] rules permit — they constrain
    /// cross-section and offsets, not which end attaches.
    ///
    /// Q and u are odd under the flip and transform as `dir·q`; the momentum
    /// flux Q·u + g·I₁ is even, which is why only the momentum contribution
    /// carries the sign in the cell update and the mass contribution does not.
    std::vector<int8_t> face_dir_l;
    std::vector<int8_t> face_dir_r;

    /// 1 when the face splices two conduits at a virtual junction. Purely
    /// informational (the scheme treats it as any other interior face) — the
    /// equivalence test and the reporting path read it.
    std::vector<uint8_t> face_virtual;

    // -----------------------------------------------------------------------
    // Conduit → cell map. Cells of a conduit are CONTIGUOUS by construction,
    // so a begin/count pair is a complete (and cheaper) CSR.
    // -----------------------------------------------------------------------

    std::vector<int> conduit_cell_begin; ///< first cell of conduit row r
    std::vector<int> conduit_cell_count; ///< cells in conduit row r
    std::vector<int> conduit_link;       ///< base LinkData index of conduit row r

    // -----------------------------------------------------------------------
    // Cell chains (CSR). A chain is a maximal run of cells joined by INTERIOR
    // faces, so it spans virtual junctions — a spliced pair of conduits is one
    // chain, which is exactly the property that makes a virtual junction
    // indistinguishable from an interior cut.
    //
    // Chains give the solver an ordered 1D stencil: the second-order scalar
    // reconstruction reads neighbours along the chain, and the implicit
    // dispersion solve is one tridiagonal system per chain (D-FV1). `dir` is
    // the cell's own axis relative to the chain's walking direction.
    // -----------------------------------------------------------------------

    std::vector<int>    chain_ptr;     ///< [n_chains + 1] row pointers
    std::vector<int>    chain_cells;   ///< cell indices in walk order
    std::vector<int8_t> chain_dir;     ///< cell axis vs. chain direction (±1)
    std::vector<int>    cell_chain;    ///< chain of each cell
    std::vector<int>    cell_chain_pos;///< position within that chain

    int n_chains() const noexcept {
        return chain_ptr.empty() ? 0 : static_cast<int>(chain_ptr.size()) - 1;
    }

    // -----------------------------------------------------------------------
    // Non-conduit links (pumps, orifices, weirs, outlets). Evaluated by their
    // existing structure equations outside the solver and applied here as
    // source/sink pairs on the two node volumes.
    // -----------------------------------------------------------------------

    std::vector<int> struct_link;  ///< LinkData index
    std::vector<int> struct_n1;    ///< upstream node
    std::vector<int> struct_n2;    ///< downstream node

    // -----------------------------------------------------------------------
    // Node → face map (CSR). Two-pass gather with NO atomics: the node update
    // reads its own face list in a fixed order, so the result is deterministic
    // and identical across backends and thread counts.
    // -----------------------------------------------------------------------

    std::vector<int>    node_face_ptr;  ///< [n_nodes + 1] row pointers
    std::vector<int>    node_face_idx;  ///< face indices
    std::vector<double> node_face_sign; ///< +1 flux ENTERS the node, −1 leaves it

    /// Bed elevation the node presents to each of its faces — the conduit
    /// invert at that end (node invert + link offset). Parallel to
    /// `node_face_idx`.
    std::vector<double> node_face_zb;

    // -----------------------------------------------------------------------
    // Node properties
    // -----------------------------------------------------------------------

    std::vector<double>  node_invert;      ///< invert elevation (ft)
    std::vector<double>  node_full_depth;  ///< crown / rim depth (ft)
    std::vector<double>  node_ponded_area; ///< ponding area above the rim (ft²)
    std::vector<double>  node_sur_depth;   ///< SURCHARGE_DEPTH above the rim (ft)

    /// Ponding eligibility, resolved at build time from the same rule DW uses
    /// (`(ALLOW_PONDING || 2D-coupled) && ponded_area > 0`). Without it the
    /// solver ponded on PONDED_AREA alone and ignored the project option.
    std::vector<uint8_t> node_can_pond;

    /// 0 regular junction, 1 virtual junction (no volume — its faces were
    /// spliced into interior faces and it owns none), 2 storage, 3 outfall.
    std::vector<uint8_t> node_kind;

    /// Storage area of a NON-storage node (junction / outfall / divider), fixed
    /// for the run. This is deliberately the ENGINE's own convention —
    /// full_volume/full_depth, i.e. MIN_SURFAREA unless a pump wet well
    /// overrode it (node::getVolume's JUNCTION branch) — so the solver's volume
    /// ledger and the volume the engine reports for the same depth are the SAME
    /// function. Any other choice makes the solver hold water the mass balance
    /// cannot see, which shows up directly as routing continuity error.
    std::vector<double> node_area;

    // -- Tabulated storage relation (STORAGE nodes only) ---------------------
    //
    // The solver must invert volume → depth every substep, and it must do so
    // without reaching back into SimulationContext (the GPU plugin has no
    // access to it). Storage geometry is therefore FLATTENED at mesh build into
    // a monotone depth→volume table sampled from node::getVolume, exactly as
    // the cross-sections are flattened for the device path.
    //
    // Junctions need none of this: their storage is linear in depth with the
    // surface area held over the routing step (plan §3.4).

    std::vector<int>    node_vol_off;   ///< offset into node_vol_tbl, −1 = none
    std::vector<double> node_vol_dmax;  ///< depth at the table top (ft)
    std::vector<double> node_vol_atop;  ///< surface area above dmax (ft²)
    std::vector<double> node_vol_tbl;   ///< kNodeVolSamples volumes per node

    // -----------------------------------------------------------------------
    // Geometry closures — one per conduit row
    // -----------------------------------------------------------------------

    std::vector<FvGeometry> geom;

    // -----------------------------------------------------------------------
    // Capacity queries
    // -----------------------------------------------------------------------

    int n_cells() const noexcept { return static_cast<int>(cell_dx.size()); }
    int n_faces() const noexcept { return static_cast<int>(face_cl.size()); }
    int n_nodes() const noexcept { return static_cast<int>(node_invert.size()); }
    int n_conduits() const noexcept {
        return static_cast<int>(conduit_cell_begin.size());
    }

    void clear() {
        cell_geom.clear(); cell_conduit.clear(); cell_dx.clear(); cell_zb.clear();
        cell_dzdx.clear();
        cell_face0.clear(); cell_face1.clear();
        cell_side0.clear(); cell_side1.clear();
        face_cl.clear(); face_cr.clear(); face_node.clear(); face_gate.clear();
        face_culvert.clear();
        face_zb.clear(); face_dx.clear(); face_virtual.clear();
        face_dir_l.clear(); face_dir_r.clear();
        conduit_cell_begin.clear(); conduit_cell_count.clear(); conduit_link.clear();
        chain_ptr.clear(); chain_cells.clear(); chain_dir.clear();
        cell_chain.clear(); cell_chain_pos.clear();
        struct_link.clear(); struct_n1.clear(); struct_n2.clear();
        node_face_ptr.clear(); node_face_idx.clear();
        node_face_sign.clear(); node_face_zb.clear();
        node_invert.clear(); node_full_depth.clear(); node_ponded_area.clear();
        node_sur_depth.clear(); node_can_pond.clear();
        node_kind.clear();
        node_area.clear();
        node_vol_off.clear(); node_vol_dmax.clear();
        node_vol_atop.clear(); node_vol_tbl.clear();
        geom.clear();
    }
};

/// Node kind codes stored in NetworkMeshData::node_kind.
enum : uint8_t {
    kNodeJunction = 0,
    kNodeVirtual  = 1,
    kNodeStorage  = 2,
    kNodeOutfall  = 3
};

/**
 * @brief Mutable solver state — the conserved variables and the node volumes.
 *
 * Separated from the mesh for the same reason SurfaceStateData is separated
 * from MeshData: the mesh is written once at init and is const to every kernel,
 * while this is what the integrator advances, what hot start serializes, and
 * what the reporting path reads.
 */
struct NetworkStateData {
    // Conserved cell variables (per BARREL — link reporting multiplies by
    // geom.barrels).
    std::vector<double> cell_a;   ///< flow area (ft²)
    std::vector<double> cell_q;   ///< discharge (cfs)

    // Derived, refreshed after every cell update so faces read a consistent
    // depth without re-inverting A(h) per face.
    std::vector<double> cell_h;   ///< depth (ft)

    // Node state. `volume` is the prognostic mass ledger — depth follows from
    // it through the node's storage relation — so node mass is conserved
    // exactly (plan §3.4).
    std::vector<double> node_volume;    ///< stored volume (ft³)
    std::vector<double> node_head;      ///< water-surface elevation (ft)
    std::vector<double> node_surf_area; ///< storage area used this routing step (ft²)
    std::vector<double> node_overflow;  ///< flooding rate this substep (cfs)

    /// Cumulative overflow VOLUME (ft³) since the last publish, so a routing
    /// step made of many substeps reports a correct mean flooding rate.
    std::vector<double> node_overflow_vol;

    // Advected scalars, cell-major: phi[s * n_cells + c]. Empty when transport
    // is off.
    std::vector<double> cell_phi;
    int n_species = 0;

    void resize(int n_cells, int n_nodes, int n_species_in) {
        const auto nc = static_cast<std::size_t>(n_cells);
        const auto nn = static_cast<std::size_t>(n_nodes);
        cell_a.assign(nc, 0.0);
        cell_q.assign(nc, 0.0);
        cell_h.assign(nc, 0.0);
        node_volume.assign(nn, 0.0);
        node_head.assign(nn, 0.0);
        node_surf_area.assign(nn, 0.0);
        node_overflow.assign(nn, 0.0);
        node_overflow_vol.assign(nn, 0.0);
        n_species = n_species_in;
        cell_phi.assign(nc * static_cast<std::size_t>(n_species_in), 0.0);
    }
};

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_NETWORK_MESH_DATA_HPP
