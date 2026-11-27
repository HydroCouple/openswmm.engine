// SPDX-License-Identifier: Apache-2.0
/**
 * @file      WebGpuKernels.hpp
 * @brief     WGSL kernels of the WebGPU 2D surface marcher (browser builds only).
 *
 * @details One macro cycle of the explicit local-inertial marcher with local time
 *          stepping, following ExplicitKokkosSurfaceSolver (advance, syncAndRebuild,
 *          refreshDt0, runMacroCycle, fireFaces, fireCells, settleAccumulatorsDev,
 *          lazySourcesDev, publishAndCopyBack) for the configurations
 *          WebGpuSurfaceSolver accepts. The host records cycles without reading
 *          anything back, so the decisions the Kokkos solver takes on the host
 *          between kernels (rebuild cadence, dt0, the tiers, the equal-substep tail,
 *          the exact landing) are taken by single-thread kernels on the device.
 *
 *          A macro cycle has nsub = 2^(K-1) base substeps (K = LTS_TIERS). Substep s
 *          fires the faces of tier k when s is a multiple of 2^k (step 2^k dt0) and
 *          the cells of tier k when s + 1 is; the host gives each substep its index
 *          through a dynamic-offset uniform (group 1).
 *
 *          Precision: WGSL has no FP64. Dynamics run in f32 on local
 *          coordinates; heads are LOCAL too: each cell keeps its free surface
 *          relative to its own reference bed, and each face carries the FP64
 *          offsets from its two cells' references to its crest and between the
 *          references, so a head difference or a flow depth is formed from small
 *          numbers (about 1e-7 m resolution instead of the ~4e-6 m of an absolute
 *          f32 elevation, which near rest drives spurious films). Water is exact: every
 *          face transfer and every source or sink is rounded once to an integer
 *          number of volume units (2^-30 m3), and cell volumes, face
 *          accumulators and applied-source ledgers are 64-bit two's-complement
 *          integers held as (lo, hi) u32 pairs. Both cells of a face book the
 *          identical integer, so conservation holds exactly. A cell that a
 *          rounding overdraft leaves below zero keeps that debt instead of
 *          being clamped (the FP64 marchers clamp, which creates the water).
 *          Float error-free transforms are not used: browsers compile WGSL with
 *          fast math, which folds them away. Transport rows (species, age,
 *          temperature) are f32 mass: a face books the same transfer into both
 *          sides, so they are conserved to the f32 rounding of each cell's landing.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */
#pragma once

#include <cstdint>

namespace openswmm::twoD::webgpu {

/// Workgroup size of every per-cell and per-face kernel (WG in the WGSL).
inline constexpr std::uint32_t kWorkgroup = 256;
/// Macro cycles between active-set rebuilds (== kRebuildEveryCycles of the other marchers).
inline constexpr std::uint32_t kRebuildEvery = 4;

/// f32 cell geometry/forcing buffer: field f of cell i is at f*nc + i. The closure fields exist
/// only under CELL_CLOSURE VFR or with quads (kCellGClosureFields then): a triangle's sorted
/// vertex beds in kZ1..kZ3, a quad's two sorted sub-triangle triples in kZ1..kZ6 with the
/// sub-triangle areas, and the vertex count. The beds are LOCAL: relative to the cell's lowest
/// bed (differences taken in FP64), so the f32 closure resolves the relief of a small cell.
/// kZref is the cell's reference bed on the datum (cz under FLAT, the lowest bed under VFR): the
/// device keeps heads relative to it and never reads it; it stays for the field numbering.
enum CellG : std::uint32_t {
    kArea,
    kZref,
    kCx,
    kCy,
    kLchar,
    kRain,
    kCoup,
    kEvap,
    kInfil,
    kCellGFields,
    kZ1 = kCellGFields,
    kZ2,
    kZ3,
    kZ4,
    kZ5,
    kZ6,
    kQuadA1,
    kQuadA2,
    kNv,
    kMann,
    kSbcX,
    kSbcY,
    kCellGClosureFields
};
/// f32 face geometry buffer: field f of face e is at f*ne + e. kOffL / kOffR: the reference bed
/// of cL / cR minus the face's crest bed (zface under MEAN, the lower endpoint bed under
/// VFR_FACE); kDref: the reference bed of cR minus that of cL; kDz (VFR_FACE only): the edge's
/// endpoint relief. All differences are taken in FP64.
enum FaceG : std::uint32_t {
    kXi,
    kOffL,
    kInvDx,
    kN2,
    kNx,
    kNy,
    kMx,
    kMy,
    kConv,
    kOffR,
    kDref,
    kFaceGFields,
    kDz = kFaceGFields,
    kFaceGVfrFields
};
/// f32 cell state buffer (kHead: the free surface relative to the cell's reference bed).
/// kQcx/kQcy: the Perot discharge vector (LOCAL_INERTIAL, DIFFUSIVE_WAVE) or the cell momentum
/// (FULL_SWE). RECONSTRUCTION_ORDER 2 adds the limited gradients of eta, depth, u and v and the
/// momentum at the start of the RK2 step (kCellFOrder2Fields), then the vertex velocity bounds
/// (4 per vertex) at Params::off_vb.
enum CellF : std::uint32_t {
    kVf,
    kHead,
    kDepth,
    kQcx,
    kQcy,
    kDtCell,
    kCellFFields,
    kGex = kCellFFields,
    kGey,
    kGhx,
    kGhy,
    kGux,
    kGuy,
    kGvx,
    kGvy,
    kQx0,
    kQy0,
    kCellFOrder2Fields
};
/// u32 cell state buffer; 64-bit integers are (lo, hi) field pairs.
enum CellU : std::uint32_t {
    kVLo,
    kVHi,
    kInfLo,
    kInfHi,
    kEvapLo,
    kEvapHi,
    kCpLo,
    kCpHi,
    kActive,
    kSeed,
    kPin,
    kTier,
    kRainLo,
    kRainHi,
    kFront,
    kTierNext,
    kV0Lo,
    kV0Hi,
    kDfLo,
    kDfHi, ///< RK2: the volume at the step's start; the step's half bookings
    kCellUFields
};
/// u32 face state buffer; kAccH*: RK2 half transfers (the same integer on both sides).
enum FaceU : std::uint32_t {
    kAccLLo,
    kAccLHi,
    kAccRLo,
    kAccRHi,
    kFTier,
    kAccHLLo,
    kAccHLHi,
    kAccHRLo,
    kAccHRHi,
    kFaceUFields
};

/// Non-WALL boundary edges ("entries", grouped in runs of one cell's consecutive edges) live in
/// tail regions of the face buffers, at the Params offsets:
///   idx   + off_bc_idx: cell (nbc) | run pointers (nruns+1) | type (nbc, rewritten per advance)
///   faceG + off_bc_g:   length | Manning n | edge midpoint x, y | slope | the cell's reference bed
///                       minus the edge's crest bed (the cell's mean bed under MEAN faces, the
///                       edge's lower endpoint bed under VFR_FACE) | the endpoint relief | the
///                       inverse centroid-to-edge distance | the prescribed stage relative to
///                       the cell's reference bed | the outward discharge per metre | the edge's
///                       conveyance | its outward normal x, y (each nbc; slope, stage,
///                       discharge and conveyance rewritten per advance)
///   faceS + off_bc_s:   q (nbc) | flux accumulated this advance (nbc) | last flow | last dt
///   faceU + off_bc_u:   inflow, outflow, requested volume since the last read (i64 pairs, 6 nbc)
enum BcG : std::uint32_t {
    kBcLength,
    kBcN,
    kBcMx,
    kBcMy,
    kBcSlope,
    kBcOff,
    kBcDz,
    kBcInvDist,
    kBcStage,
    kBcFlow,
    kBcConv,
    kBcNx,
    kBcNy,
    kBcGFields
};
enum BcS : std::uint32_t { kBcQ, kBcAccum, kBcLastFlow, kBcLastDt, kBcSFields };
enum BcU : std::uint32_t { kBcInLo, kBcInHi, kBcOutLo, kBcOutHi, kBcReqLo, kBcReqHi, kBcUFields };

/// 1D-2D junction exchange points (SurfaceStateData::node_coupling), in further tails. Each
/// point works on its own datum zp, the reference bed of its coupling cell, so the head
/// difference that drives the exchange is formed from small numbers like a face's:
///   idx   + off_cp_idx: per point (ncp each) cell | node slot | law | head mode | head stencil
///                       begin, end | availability stencil begin, end | flags; group pointers
///                       (ncp_groups+1) | group items (ncp) | stencil cells (nst) | node budget
///                       (lo, hi per node slot; rewritten per advance)
///   faceG + off_cp_g:   per point the CpG fields (ncp each), then the stencil coefficient and
///                       offset (nst each)
///   faceU + off_cp_u:   exchanged volume per point, drawn spill volume per node slot (i64 pairs)
/// Head modes: 0 the cell's head; 1 a weighted vertex head (the FLAT pseudo-Laplacian:
/// sum of w*head + kCpConst); 2 the wet-depth-weighted mean of (head + offset) over the stencil,
/// else the cell's head (flag 2) or kCpFall.
enum CpI : std::uint32_t {
    kCpCell,
    kCpNode,
    kCpLaw,
    kCpMode,
    kCpHb,
    kCpHe,
    kCpDb,
    kCpDe,
    kCpFlags,
    kCpIFields
};
enum CpG : std::uint32_t {
    kCpCd,
    kCpArea,
    kCpZc,
    kCpLw,
    kCpAo,
    kCpCw,
    kCpCsw,
    kCpCoi,
    kCpCoo,
    kCpHeps,
    kCpCrown,
    kCpConst,
    kCpFall,
    kCpH1,
    kCpD1,
    kCpEq,
    kCpGFields
};
inline constexpr std::uint32_t kCpFlagEqualize = 1u, kCpFlagFallbackCell = 2u;

/// FULL_SWE: the exterior WALL edges of each cell, whose pressure the cell books when it fires.
///   idx   + off_wall_ptr: per cell the first wall (nc+1)
///   faceG + off_wall_g:   outward normal x, y | length | midpoint x, y (each nw)
/// and the face-side momentum accumulators in faceS + off_mom: fmxL | fmyL | fmxR | fmyR (each ne).
enum WallG : std::uint32_t { kWallNx, kWallNy, kWallLen, kWallMx, kWallMy, kWallGFields };
/// kCpCell of a point that never fires (no coupling cell).
inline constexpr std::uint32_t kCpNoCell = 0xffffffffu;

/// Uniform parameters; layout matches `struct Params` in the WGSL.
struct Params {
    float g, dry, theta, froude, beta3, alpha, dt_max, h_on, h_off, theta_ref, span, mwf;
    std::uint32_t nc, ne, nslots, flags, rebuild_every, off_ptr, off_edge, nbc;
    std::uint32_t nruns, off_bc_idx, off_bc_g, off_bc_s, off_bc_u, tiers, nsub, ncp;
    std::uint32_t ncp_groups, ncp_nodes, nst, off_cp_idx, off_cp_g, off_cp_u, off_wall_ptr,
        off_wall_g;
    float beta, dh_eps, dispersion, pad4;
    std::uint32_t nw, off_mom, nv, off_vs_ptr;
    /// RECONSTRUCTION_ORDER 2: idx + off_vs_ptr: vertex stencil pointers (nv+1), cells; faceG +
    /// off_vs_off: per stencil entry the cell's reference bed minus the vertex bed; idx +
    /// off_cell_v: each cell's vertices (4 per cell, padded); faceG + off_cell_arm: per cell
    /// vertex k its arm x, y and the cell's reference bed minus the vertex bed (3 x 4 nc,
    /// field-major by k); faceG + off_csr_arm: per incidence entry the face midpoint minus the
    /// centroid, x | y; cellF + off_vb: vertex velocity bounds.
    std::uint32_t off_vs_off, off_cell_v, off_cell_arm, off_csr_arm;
    std::uint32_t off_vb, ncsr, off_ph_u, off_ph_f;
    /// Momentum physics: idx + off_wd_idx: drag edges' cells (nwd) | per-cell group pointers
    /// (nwd_groups+1); faceG + off_wd_g: nx | ny | length | Cw | reference bed minus the lower
    /// endpoint bed | endpoint relief (each nwd); faceG + off_walld: wall distance (nc).
    std::uint32_t nwd, nwd_groups, off_wd_idx, off_wd_g;
    /// Transport rows (ns of them; none when ns is 0): the faceS regions of kSp* below, the
    /// kSpFlag* bits, the temperature and age rows (kSpNoRow when absent) and the ledger fold's
    /// workgroup count.
    std::uint32_t off_walld, off_sp, ns, sp_flags;
    std::uint32_t temp_row, age_row, off_sp_b, off_sp_c;
    std::uint32_t off_sp_acc, off_sp_in, sp_groups, sp_pad;
};
static_assert(sizeof(Params) == 272, "Params must match the WGSL uniform");

/// Largest LTS_TIERS (== the other marchers' clamp).
inline constexpr std::uint32_t kMaxTiers = 8;

/// Device control block; layout matches `struct Ctl` in the WGSL.
struct Ctl {
    float thi, tlo, dt, dt0, dt_tail, sync_hi, sync_lo, last_dt;
    std::uint32_t mode, tail_left, cycles, steps, do_rebuild, do_fire, do_lazy, n_active_last;
    std::uint32_t face_steps, n_faces_last, finishing, finished, n_active, n_faces, dt_min_bits;
    std::uint32_t breach; ///< FRONT_REBUILD: a frontier cell wetted since the last rebuild
    std::uint32_t cells_tier[kMaxTiers];      ///< active cells per tier, counted at a rebuild
    std::uint32_t faces_tier[kMaxTiers];      ///< marched faces per tier, counted at a rebuild
    std::uint32_t faces_tier_last[kMaxTiers]; ///< faces_tier adopted by the last rebuild
    std::uint32_t occupancy[kMaxTiers];       ///< cells_tier summed over this advance's rebuilds
    std::uint32_t n_seed;                     ///< rebuild seeds (FRONT_REBUILD rings)
    /// Momentum physics reductions: the wave-CFL minimum and the mixing-rate maximum (float bits),
    /// a failure flag, and the deterministic energy sum.
    std::uint32_t ph_stable_bits, ph_rate_bits, ph_bad;
    float ph_energy, ph_pad;
    /// Momentum physics parameters of the next dispatches, written by the host: the rotation's
    /// cosine and sine, the drag interval, the mixing step, Cs, the background viscosity and the
    /// mixing-length coefficient.
    float ph_cos, ph_sin, ph_seconds, ph_dt, ph_cs, ph_bg, ph_dc, ph_pad2;
    /// Transport rows: dispersion limiter binds since the host last reset the count.
    std::uint32_t sp_binds, sp_pad;
};
static_assert(sizeof(Ctl) == 288, "Ctl must match the WGSL storage struct");

inline constexpr std::uint32_t kFlagPerot = 1u, kFlagAdvection = 2u, kFlagTimeBlend = 4u;
inline constexpr std::uint32_t kFlagVfr = 8u, kFlagVfrFace = 16u, kFlagQuads = 32u;
inline constexpr std::uint32_t kFlagDiffusive = 64u, kFlagSwe = 128u, kFlagFront = 256u;
inline constexpr std::uint32_t kFlagOrder2 = 512u, kFlagPhysics = 1024u;
/// Momentum physics work arrays: cellU + off_ph_u: stencil cells (5 nc) | stencil size (nc) |
/// face connected (ne); cellF + off_ph_f: kPh* fields (nc each), stencil gx | gy (5 nc each),
/// face stress tx | ty | bounds bx | by (ne each), energy partials (one per workgroup).
enum PhF : std::uint32_t {
    kPhX0,
    kPhY0,
    kPhRx,
    kPhRy,
    kPhSx,
    kPhSy,
    kPhTx,
    kPhTy,
    kPhVisc,
    kPhTerm,
    kPhHeldH,
    kPhHeldEta,
    kPhHeldQx,
    kPhHeldQy,
    kPhFields
};
/// FRONT_REBUILD: halo rings (== kFrontHaloRings of the other marchers) and the Jacobi rounds of
/// the dry-halo tier pull-down (an even count, so the result lands back in kTier).
inline constexpr std::uint32_t kFrontHaloRings = 5, kPullRounds = 8;
/// Momentum physics: parallel rounds of the corner-stencil rule before the serial remainder.
inline constexpr std::uint32_t kChainRounds = 16;

/// Transport rows (species, age, temperature; KokkosSpeciesKernels.hpp), f32 mass in faceS
/// regions at the Params offsets (G = sp_groups, one per 256 cells):
///   off_sp:     cell mass (row-major, ns nc) | runoff volume (nc)
///   off_sp_b:   ledger fold partials (4 ns G; rain, coupling in, infiltration, coupling out by
///               row) | boundary gain | loss | requested | applied mass (nbc ns each) | per
///               junction point drained mass (ncp ns) | spill volume (ncp) | spill mass (ncp ns);
///               read back and cleared every round
///   off_sp_c:   per-cell ledgers (4 ns nc, the fold partials' rows) | coupling source density
///               (ns nc)
///   off_sp_acc: face accumulators left | right (ns ne each) | per face the crest bed minus the
///               centroid crest zface (ne; FULL_SWE's dispersion depth under VFR_FACE)
///   off_sp_in:  boundary inflow concentrations (nbc ns) | node concentrations per node slot
///               (nodes ns) | rain concentrations (ns) | RECONSTRUCTION_ORDER 2: the mass at the
///               step's start (ns nc)
/// A face books the same f32 transfer into both sides; rounding enters when a cell lands it.
inline constexpr std::uint32_t kSpFlagCsrc = 1u, kSpFlagRunoff = 2u, kSpFlagNodeConc = 4u;
inline constexpr std::uint32_t kSpNoRow = 0xffffffffu;
inline constexpr std::uint32_t kSpLedgers = 4;
/// Ctl::mode values.
inline constexpr std::uint32_t kModeMarch = 0u, kModeTail = 1u, kModeLanded = 2u;

inline constexpr const char* kSurfaceWgsl = R"WGSL(
struct Params {
  g: f32, dry: f32, theta: f32, froude: f32,
  beta3: f32, alpha: f32, dtMax: f32, hOn: f32,
  hOff: f32, thetaRef: f32, span: f32, mwf: f32,
  nc: u32, ne: u32, nslots: u32, flags: u32,
  rebuildEvery: u32, offPtr: u32, offEdge: u32, nbc: u32,
  nruns: u32, offBcIdx: u32, offBcG: u32, offBcS: u32,
  offBcU: u32, tiers: u32, nsub: u32, ncp: u32,
  ncpGroups: u32, ncpNodes: u32, nst: u32, offCpIdx: u32,
  offCpG: u32, offCpU: u32, offWallPtr: u32, offWallG: u32,
  beta: f32, dhEps: f32, dispersion: f32, pad4: f32,
  nw: u32, offMom: u32, nv: u32, offVsPtr: u32,
  offVsOff: u32, offCellV: u32, offCellArm: u32, offCsrArm: u32,
  offVb: u32, ncsr: u32, offPhU: u32, offPhF: u32,
  nwd: u32, nwdGroups: u32, offWdIdx: u32, offWdG: u32,
  offWalld: u32, offSp: u32, ns: u32, spFlags: u32,
  tempRow: u32, ageRow: u32, offSpB: u32, offSpC: u32,
  offSpAcc: u32, offSpIn: u32, spGroups: u32, spPad: u32,
};
// mode: 0 marching, 1 equal-substep tail, 2 landed. The clock (thi integer seconds, tlo the
// fraction) and the lazy-source sync time are relative to the start of the advance.
struct Ctl {
  thi: f32, tlo: f32, dt: f32, dt0: f32,
  dtTail: f32, syncHi: f32, syncLo: f32, lastDt: f32,
  mode: u32, tailLeft: u32, cycles: u32, steps: u32,
  doRebuild: u32, doFire: u32, doLazy: u32, nActiveLast: u32,
  faceSteps: u32, nFacesLast: u32, finishing: u32, finished: u32,
  nActive: atomic<u32>, nFaces: atomic<u32>, dtMinBits: atomic<u32>, breach: atomic<u32>,
  cellsTier: array<atomic<u32>, 8>,
  facesTier: array<atomic<u32>, 8>,
  facesTierLast: array<u32, 8>,
  occupancy: array<u32, 8>,
  nSeed: atomic<u32>,
  phStableBits: atomic<u32>, phRateBits: atomic<u32>, phBad: atomic<u32>,
  phEnergy: f32, phPad: f32,
  phCos: f32, phSin: f32, phSeconds: f32, phDt: f32, phCs: f32, phBg: f32, phDc: f32, phPad2: f32,
  spBinds: atomic<u32>, spPad: u32,
};
@group(0) @binding(0) var<uniform> P: Params;
@group(0) @binding(1) var<storage, read_write> C: Ctl;
@group(0) @binding(2) var<storage, read> cellG: array<f32>;
@group(0) @binding(3) var<storage, read> faceG: array<f32>;
// idx: cL (ne) | cR (ne) | slotL (ne) | slotR (ne) | cellPtr (nc+1) | cellEdge (2ne, bit 31: cell
// is cR) | boundary entries (see the host header). faceG, faceS and faceU also carry boundary tails.
@group(0) @binding(4) var<storage, read> idx: array<u32>;
@group(0) @binding(5) var<storage, read_write> cellF: array<f32>;
@group(0) @binding(6) var<storage, read_write> cellU: array<u32>;
// faceS: q (ne) | published face flux (ne; zero where a cell is inactive)
@group(0) @binding(7) var<storage, read_write> faceS: array<f32>;
@group(0) @binding(8) var<storage, read_write> faceU: array<u32>;
// The base substep of the macro cycle a face/cell/boundary dispatch belongs to (x), and which
// cells a cells dispatch fires (y): 0 every due tier; 1 tier 0 only; 2 tiers 1 and up only.
// With junction exchange points the cells fire as 1, then the boundary and the exchange, then
// 2 (the Kokkos order: the tier-0 firing, its boundary and exchange, then the slower tiers).
@group(1) @binding(0) var<uniform> S: vec4<u32>;

const WG: u32 = 256u;
const UNITS_PER_M3: f32 = 1073741824.0;        // 2^30
const M3_PER_UNIT: f32 = 9.313225746154785e-10; // 2^-30
const ETA_DEADBAND: f32 = 1.0e-12;
const FLAG_PEROT: u32 = 1u;
const FLAG_ADVECTION: u32 = 2u;
const FLAG_TIME_BLEND: u32 = 4u;
const FLAG_VFR: u32 = 8u;
const FLAG_VFR_FACE: u32 = 16u;
const FLAG_QUADS: u32 = 32u;
const FLAG_DIFFUSIVE: u32 = 64u;
const FLAG_SWE: u32 = 128u;
const FLAG_FRONT: u32 = 256u;
const FLAG_ORDER2: u32 = 512u;
const FLAG_PHYSICS: u32 = 1024u;
const SP_CSRC: u32 = 1u;
const SP_RUNOFF: u32 = 2u;
const SP_NODE_CONC: u32 = 4u;
const VFR_FLAT_RELIEF: f32 = 1.0e-9;
// A Newton step this small relative to the cell's relief is a few f32 ulps of the local stage:
// converged (the FP64 marchers use 1e-12 absolute).
const NEWTON_TOL: f32 = 4.8e-7;

fn cg(f: u32, i: u32) -> f32 { return cellG[f * P.nc + i]; }
fn fg(f: u32, e: u32) -> f32 { return faceG[f * P.ne + e]; }
fn cf(f: u32, i: u32) -> f32 { return cellF[f * P.nc + i]; }
fn setCf(f: u32, i: u32, v: f32) { cellF[f * P.nc + i] = v; }
fn cu(f: u32, i: u32) -> u32 { return cellU[f * P.nc + i]; }
fn setCu(f: u32, i: u32, v: u32) { cellU[f * P.nc + i] = v; }
fn cL(e: u32) -> u32 { return idx[e]; }
fn cR(e: u32) -> u32 { return idx[P.ne + e]; }
fn slotL(e: u32) -> u32 { return idx[2u * P.ne + e]; }
fn slotR(e: u32) -> u32 { return idx[3u * P.ne + e]; }
fn rowBegin(i: u32) -> u32 { return idx[P.offPtr + i]; }
fn rowEnd(i: u32) -> u32 { return idx[P.offPtr + i + 1u]; }
fn rowCode(p: u32) -> u32 { return idx[P.offEdge + p]; }

// ---- 64-bit two's-complement integers as (lo, hi) ---------------------------
fn add64(a: vec2<u32>, b: vec2<u32>) -> vec2<u32> {
  let lo = a.x + b.x;
  return vec2<u32>(lo, a.y + b.y + select(0u, 1u, lo < a.x));
}
fn neg64(a: vec2<u32>) -> vec2<u32> {
  let lo = ~a.x + 1u;
  return vec2<u32>(lo, ~a.y + select(0u, 1u, lo == 0u));
}
fn isNeg64(a: vec2<u32>) -> bool { return (a.y & 0x80000000u) != 0u; }
fn isZero64(a: vec2<u32>) -> bool { return a.x == 0u && a.y == 0u; }
// x holds an integer value, |x| < 2^62; exact.
fn fromUnits(x: f32) -> vec2<u32> {
  let ax = abs(x);
  let hi = floor(ax * 2.3283064365386963e-10);
  let lo = ax - hi * 4294967296.0;
  let r = vec2<u32>(u32(lo), u32(hi));
  if (x < 0.0) { return neg64(r); }
  return r;
}
fn toM3(a: vec2<u32>) -> f32 {
  var v = a; var s = 1.0;
  if (isNeg64(a)) { v = neg64(a); s = -1.0; }
  return s * (f32(v.y) * 4.0 + f32(v.x) * M3_PER_UNIT);
}
fn quantize(m3: f32) -> f32 { return round(m3 * UNITS_PER_M3); }
fn vol64(i: u32) -> vec2<u32> { return vec2<u32>(cu(0u, i), cu(1u, i)); }
fn addCellU(f: u32, i: u32, v: vec2<u32>) {
  let r = add64(vec2<u32>(cu(f, i), cu(f + 1u, i)), v);
  setCu(f, i, r.x); setCu(f + 1u, i, r.y);
}
fn accL(e: u32) -> vec2<u32> { return vec2<u32>(faceU[e], faceU[P.ne + e]); }
fn accR(e: u32) -> vec2<u32> { return vec2<u32>(faceU[2u * P.ne + e], faceU[3u * P.ne + e]); }
fn setAccL(e: u32, v: vec2<u32>) { faceU[e] = v.x; faceU[P.ne + e] = v.y; }
fn setAccR(e: u32, v: vec2<u32>) { faceU[2u * P.ne + e] = v.x; faceU[3u * P.ne + e] = v.y; }
fn ftier(e: u32) -> u32 { return faceU[4u * P.ne + e]; }
fn setFtier(e: u32, t: u32) { faceU[4u * P.ne + e] = t; }
// RECONSTRUCTION_ORDER 2 (RK2): every stage booking T also books H = T >> 1 (rounded down; the
// same H, with opposite signs, on both sides of a face) into the step's half accumulators and
// ledgers, so the step ends exactly at V0 + the sum of halves (W5 notes, decision 5).
fn order2() -> bool { return (P.flags & FLAG_ORDER2) != 0u; }
fn half64(a: vec2<u32>) -> vec2<u32> { return vec2<u32>((a.x >> 1u) | (a.y << 31u), u32(i32(a.y) >> 1u)); }
fn accHL(e: u32) -> vec2<u32> { return vec2<u32>(faceU[5u * P.ne + e], faceU[6u * P.ne + e]); }
fn accHR(e: u32) -> vec2<u32> { return vec2<u32>(faceU[7u * P.ne + e], faceU[8u * P.ne + e]); }
fn setAccHL(e: u32, v: vec2<u32>) { faceU[5u * P.ne + e] = v.x; faceU[6u * P.ne + e] = v.y; }
fn setAccHR(e: u32, v: vec2<u32>) { faceU[7u * P.ne + e] = v.x; faceU[8u * P.ne + e] = v.y; }


// ---- the volume <-> free-surface closures (VfrClosure.hpp, QuadVfr.hpp) ---------
fn cbrtPos(x: f32) -> f32 { if (x <= 0.0) { return 0.0; } return pow(x, 1.0 / 3.0); }
// Inputs sorted z1 <= z2 <= z3: the wetted fraction, d(mean depth)/d(eta).
fn vfrWet(z1: f32, z2: f32, z3: f32, eta: f32) -> f32 {
  let relief = z3 - z1;
  if (relief < VFR_FLAT_RELIEF) { return select(0.0, 1.0, eta > z1); }
  if (eta <= z1) { return 0.0; }
  if (eta >= z3) { return 1.0; }
  if (eta <= z2) {
    let d21 = z2 - z1;
    if (d21 < VFR_FLAT_RELIEF) { return 0.0; }
    let t = eta - z1;
    return t * t / (d21 * relief);
  }
  let t = z3 - eta;
  return 1.0 - t * t / (relief * (z3 - z2));
}
fn vfrHExact(z1: f32, z2: f32, z3: f32, eta: f32) -> f32 {
  let zbar = (z1 + z2 + z3) / 3.0;
  let relief = z3 - z1;
  if (relief < VFR_FLAT_RELIEF) { return max(eta - zbar, 0.0); }
  if (eta <= z1) { return 0.0; }
  if (eta >= z3) { return eta - zbar; }
  if (eta <= z2) {
    let d21 = z2 - z1;
    if (d21 < VFR_FLAT_RELIEF) { return 0.0; }
    let t = eta - z1;
    return t * t * t / (3.0 * d21 * relief);
  }
  let t = z3 - eta;
  return (eta - zbar) + t * t * t / (3.0 * relief * (z3 - z2));
}
fn vfrStage(z1: f32, z2: f32, z3: f32, eps: f32) -> f32 {
  let relief = z3 - z1;
  let d21 = z2 - z1;
  if (eps <= d21 / relief) { return z1 + sqrt(eps * d21 * relief); }
  return z3 - sqrt((1.0 - eps) * relief * (z3 - z2));
}
// Mean depth from eta, with the eps-regularized linear tail below the switch stage.
fn vfrH(z1: f32, z2: f32, z3: f32, eta: f32, eps: f32) -> f32 {
  let relief = z3 - z1;
  if (eps <= 0.0 || relief < VFR_FLAT_RELIEF) { return vfrHExact(z1, z2, z3, eta); }
  let etaS = vfrStage(z1, z2, z3, eps);
  if (eta >= etaS) { return vfrHExact(z1, z2, z3, eta); }
  return max(vfrHExact(z1, z2, z3, etaS) - eps * (etaS - eta), 0.0);
}
// Eta from mean depth (vfrEtaFromMeanDepth): flat and fully wet exact, the regularized tail,
// the closed-form lower branch, then a safeguarded Newton on the upper branch.
fn vfrEta(z1: f32, z2: f32, z3: f32, h: f32, eps: f32) -> f32 {
  let zbar = (z1 + z2 + z3) / 3.0;
  let relief = z3 - z1;
  if (relief < VFR_FLAT_RELIEF) { return zbar + max(h, 0.0); }
  if (h >= z3 - zbar) { return zbar + h; }
  if (eps > 0.0) {
    let etaS = vfrStage(z1, z2, z3, eps);
    let hS = vfrHExact(z1, z2, z3, etaS);
    let hh = max(h, 0.0);
    if (hh <= hS) { return etaS - (hS - hh) / eps; }
  } else if (!(h > 0.0)) {
    return z1;
  }
  let d21 = z2 - z1;
  if (h <= d21 * d21 / (3.0 * relief)) { return z1 + cbrtPos(3.0 * h * d21 * relief); }
  if (z3 - z2 < VFR_FLAT_RELIEF) { return zbar + h; }
  let d32 = z3 - z2;
  let denom = 3.0 * relief * d32;
  var lo = z2; var hi = z3;
  var eta = zbar + h;
  if (eta <= lo || eta >= hi) { eta = 0.5 * (lo + hi); }
  for (var it = 0; it < 40; it = it + 1) {
    let dz3 = z3 - eta;
    let f = (eta - zbar) + dz3 * dz3 * dz3 / denom - h;
    if (f > 0.0) { hi = eta; } else { lo = eta; }
    let df = 1.0 - dz3 * dz3 / (relief * d32);
    var next = 0.5 * (lo + hi);
    if (df > 1.0e-12) { next = eta - f / df; }
    if (next <= lo || next >= hi) { next = 0.5 * (lo + hi); }
    if (abs(next - eta) <= NEWTON_TOL * relief) { return next; }
    eta = next;
  }
  return eta;
}
fn vfrSlope(z1: f32, z2: f32, z3: f32, eta: f32, eps: f32) -> f32 {
  if (eps <= 0.0 || z3 - z1 < VFR_FLAT_RELIEF) { return vfrWet(z1, z2, z3, eta); }
  if (eta >= vfrStage(z1, z2, z3, eps)) { return vfrWet(z1, z2, z3, eta); }
  return select(0.0, eps, vfrH(z1, z2, z3, eta, eps) > 0.0);
}
// A quad's two-plane closure: the area-weighted sum of its two sub-triangles.
fn quadH(i: u32, eta: f32, eps: f32) -> f32 {
  let a1 = cg(15u, i); let a2 = cg(16u, i);
  let d1 = vfrH(cg(9u, i), cg(10u, i), cg(11u, i), eta, eps);
  let d2 = vfrH(cg(12u, i), cg(13u, i), cg(14u, i), eta, eps);
  return (a1 * d1 + a2 * d2) / (a1 + a2);
}
fn quadSlope(i: u32, eta: f32, eps: f32) -> f32 {
  let a1 = cg(15u, i); let a2 = cg(16u, i);
  let s1 = vfrSlope(cg(9u, i), cg(10u, i), cg(11u, i), eta, eps);
  let s2 = vfrSlope(cg(12u, i), cg(13u, i), cg(14u, i), eta, eps);
  return (a1 * s1 + a2 * s2) / (a1 + a2);
}
fn quadEta(i: u32, h: f32, eps: f32) -> f32 {
  let a1 = cg(15u, i); let a2 = cg(16u, i);
  let z0 = cg(9u, i); let z1 = cg(10u, i); let z2 = cg(11u, i);
  let z3 = cg(12u, i); let z4 = cg(13u, i); let z5 = cg(14u, i);
  let zw = (a1 * (z0 + z1 + z2) / 3.0 + a2 * (z3 + z4 + z5) / 3.0) / (a1 + a2);
  let ztop = max(z2, z5);
  let relief = ztop - min(z0, z3);
  if (relief < VFR_FLAT_RELIEF) { return zw + max(h, 0.0); }
  if (h >= ztop - zw) { return zw + h; }
  let lo0 = min(vfrEta(z0, z1, z2, 0.0, eps), vfrEta(z3, z4, z5, 0.0, eps));
  if (!(h > 0.0)) { return lo0; }
  var lo = lo0; var hi = ztop;
  var eta = zw + h;
  if (eta <= lo || eta >= hi) { eta = 0.5 * (lo + hi); }
  for (var it = 0; it < 48; it = it + 1) {
    let f = quadH(i, eta, eps) - h;
    if (f > 0.0) { hi = eta; } else { lo = eta; }
    let df = quadSlope(i, eta, eps);
    var next = 0.5 * (lo + hi);
    if (df > 1.0e-12) { next = eta - f / df; }
    if (next <= lo || next >= hi) { next = 0.5 * (lo + hi); }
    if (abs(next - eta) <= NEWTON_TOL * relief) { return next; }
    eta = next;
  }
  return eta;
}
fn isQuad(i: u32) -> bool { return (P.flags & FLAG_QUADS) != 0u && cg(17u, i) == 4.0; }
// The free surface of a cell holding mean depth d, relative to its reference bed
// (inertial::etaDepthScalar / etaDepthQuadScalar): d under FLAT, the local VFR stage otherwise.
fn cellEta(i: u32, d: f32) -> f32 {
  if ((P.flags & FLAG_VFR) == 0u) { return d; }
  if (isQuad(i)) { return quadEta(i, d, P.mwf); }
  return vfrEta(cg(9u, i), cg(10u, i), cg(11u, i), d, P.mwf);
}
// B&S Eq. 14 wetted-edge depth of a surface over an edge with endpoint beds zlo <= zhi.
fn faceDepthFromEta(eta: f32, zlo: f32, zhi: f32) -> f32 {
  if (eta <= zlo) { return 0.0; }
  let dz = zhi - zlo;
  if (dz < 1.0e-9) { return eta - zlo; }
  if (eta <= zhi) { let t = eta - zlo; return t * t / (2.0 * dz); }
  return eta - 0.5 * (zlo + zhi);
}
// Interior face flow depth from the two local heads: MEAN (the higher surface over the
// centroid-bed crest) or VFR_FACE (B&S Eq. 14 over the edge's endpoint beds).
fn faceFlowDepth(e: u32, etaA: f32, etaB: f32) -> f32 {
  let s = max(etaA + fg(1u, e), etaB + fg(9u, e)); // the higher surface above the crest bed
  if ((P.flags & FLAG_VFR_FACE) == 0u) { return s; }
  return faceDepthFromEta(s, 0.0, fg(11u, e));
}

// ---- transport rows (KokkosSpeciesKernels.hpp; f32 mass, host header layout) -------------
// Every total a kernel books goes into a slot its cell, face, boundary entry or junction point
// owns. `w` weighs a booking into a total: 1, or 1/2 inside an RK2 stage (the step is the mean
// of its start and its second stage, as the FP64 marchers blend their totals).
fn spMass(s: u32, i: u32) -> f32 { return faceS[P.offSp + s * P.nc + i]; }
fn setSpMass(s: u32, i: u32, v: f32) { faceS[P.offSp + s * P.nc + i] = v; }
fn spAdd(at: u32, v: f32) { faceS[at] = faceS[at] + v; }
fn spRunoff(i: u32, v: f32) {
  if ((P.spFlags & SP_RUNOFF) != 0u) { spAdd(P.offSp + P.ns * P.nc + i, v); }
}
fn spLed(k: u32, s: u32, i: u32) -> u32 { return P.offSpC + (k * P.ns + s) * P.nc + i; }
fn spBc(f: u32, k: u32, s: u32) -> u32 {
  return P.offSpB + 4u * P.ns * P.spGroups + (f * P.nbc + k) * P.ns + s;
}
fn spCpMass(k: u32, s: u32) -> u32 {
  return P.offSpB + 4u * P.ns * P.spGroups + 4u * P.nbc * P.ns + k * P.ns + s;
}
fn spCpSpill(k: u32) -> u32 { return spCpMass(P.ncp, 0u) + k; }
fn spCpGain(k: u32, s: u32) -> u32 { return spCpMass(P.ncp, 0u) + P.ncp + k * P.ns + s; }
fn spAccL(s: u32, e: u32) -> u32 { return P.offSpAcc + s * P.ne + e; }
fn spAccR(s: u32, e: u32) -> u32 { return P.offSpAcc + (P.ns + s) * P.ne + e; }
fn spBcConc(k: u32, s: u32) -> f32 { return faceS[P.offSpIn + k * P.ns + s]; }
fn spNodeConc(n: u32, s: u32) -> f32 { return faceS[P.offSpIn + (P.nbc + n) * P.ns + s]; }
fn spRainConc(s: u32) -> f32 { return faceS[P.offSpIn + (P.nbc + P.ncpNodes) * P.ns + s]; }
fn spMass0(s: u32, i: u32) -> u32 {
  return P.offSpIn + (P.nbc + P.ncpNodes + 1u) * P.ns + s * P.nc + i;
}
// The concentration water leaving cell i carries (donorConc).
fn spConc(s: u32, i: u32) -> f32 {
  let v = cf(0u, i);
  if (!(v > 0.0)) { return 0.0; }
  return spMass(s, i) / v;
}
// One face's advective and dispersive row transfers (bookFace): dM is the water moved cL to cR,
// hf the dispersive conductance's depth, kf the face tier, cfac the LOCAL_INERTIAL conveyance.
fn spFace(e: u32, dM: f32, hf: f32, dt: f32, kf: u32, cfac: f32) {
  let a = cL(e); let b = cR(e);
  if (dM != 0.0) {
    let donor = select(b, a, dM > 0.0);
    for (var s = 0u; s < P.ns; s = s + 1u) {
      let c = spConc(s, donor);
      if (c == 0.0) { continue; }
      let dMs = dM * c;
      spAdd(spAccL(s, e), -dMs);
      spAdd(spAccR(s, e), dMs);
    }
  }
  if (!(P.dispersion > 0.0)) { return; }
  let va = cf(0u, a); let vb = cf(0u, b);
  if (!(va > 0.0 && vb > 0.0)) { return; }
  let cond = P.dispersion * hf * fg(0u, e) * fg(2u, e) * dt * cfac; // m3 exchanged
  for (var s = 0u; s < P.ns; s = s + 1u) {
    let ma = spMass(s, a); let mb = spMass(s, b);
    let ca = ma / va; let cb = mb / vb;
    var dMd = cond * (ca - cb); // positive: a to b
    if (dMd == 0.0) { continue; }
    let giver = select(a, b, dMd < 0.0);
    let receiver = select(b, a, dMd < 0.0);
    let eq = (ca - cb) * va * vb / (va + vb) / select(3.0, 4.0, isQuad(receiver));
    var refire = 1.0;
    if (C.mode == 0u) { refire = f32(1u << (cu(11u, giver) - kf)); }
    let share = select(P.beta3, P.beta3 * 0.75, isQuad(giver)) / refire * abs(select(ma, mb, dMd < 0.0));
    var cap = abs(eq);
    if (s != P.tempRow) { cap = min(abs(eq), share); }
    if (abs(dMd) > cap) {
      dMd = select(cap, -cap, dMd < 0.0);
      atomicAdd(&C.spBinds, 1u);
    }
    spAdd(spAccL(s, e), -dMd);
    spAdd(spAccR(s, e), dMd);
  }
}
// Gather and clear cell i's side of every incident face accumulator of row s.
fn spGather(s: u32, i: u32) -> f32 {
  var dm = 0.0;
  for (var p = rowBegin(i); p < rowEnd(i); p = p + 1u) {
    let code = rowCode(p);
    let e = code & 0x7fffffffu;
    var at = spAccL(s, e);
    if ((code & 0x80000000u) != 0u) { at = spAccR(s, e); }
    dm = dm + faceS[at];
    faceS[at] = 0.0;
  }
  return dm;
}
// A firing cell lands its pending row transfers with the water it lands (flux, m3).
fn spLand(i: u32, flux: f32, w: f32) {
  spRunoff(i, -w * flux);
  for (var s = 0u; s < P.ns; s = s + 1u) {
    var m = spMass(s, i) + spGather(s, i);
    if (s != P.tempRow && m < 0.0) { m = 0.0; }
    setSpMass(s, i, m);
  }
}
// A cell leaving its cadence lands what is pending (settleAccumulators).
fn spSettle(i: u32) {
  for (var s = 0u; s < P.ns; s = s + 1u) {
    let dm = spGather(s, i);
    if (dm != 0.0) {
      var m = spMass(s, i) + dm;
      if (m < 0.0 && s != P.tempRow) { m = 0.0; }
      setSpMass(s, i, m);
    }
  }
}
// The accepted sources of one firing mix into every row, and the sinks leave at the mixed
// concentration (applySources): rain and incoming coupling volume, the signed coupling request,
// the availability scale and volume, the applied infiltration and evaporation, area times dt.
fn spSources(i: u32, rain: f32, coupling: f32, scale: f32, available: f32, inf: f32, evap: f32,
             adt: f32, w: f32) {
  let out = max(-coupling, 0.0) * scale;
  spRunoff(i, w * (out - max(coupling, 0.0)));
  let csrc = coupling > 0.0 && (P.spFlags & SP_CSRC) != 0u;
  for (var s = 0u; s < P.ns; s = s + 1u) {
    var m = spMass(s, i);
    let rainMass = rain * spRainConc(s);
    var couplingMass = 0.0;
    if (csrc) { couplingMass = faceS[P.offSpC + (4u * P.ns + s) * P.nc + i] * adt; }
    m = m + rainMass + couplingMass;
    var conc = 0.0;
    if (available > 0.0) { conc = m / available; }
    let mi = conc * inf;
    let mc = conc * out;
    var me = 0.0;
    if (s == P.ageRow || s == P.tempRow) { me = conc * evap; }
    m = m - (mi + mc + me);
    if (s != P.tempRow && m < 0.0) { m = 0.0; }
    setSpMass(s, i, m);
    spAdd(spLed(0u, s, i), w * rainMass);
    spAdd(spLed(1u, s, i), w * couplingMass);
    spAdd(spLed(2u, s, i), w * mi);
    spAdd(spLed(3u, s, i), w * mc);
  }
}
// Water leaving cell i (dv m3, through an open edge or a drain) takes its rows along at the
// cell's concentration (sinkMassAtCellConc), booked as boundary entry k's loss (bc != 0) or
// junction point k's drained mass.
fn spSink(i: u32, dv: f32, w: f32, bc: u32, k: u32) {
  if (!(dv > 0.0)) { return; }
  spRunoff(i, w * dv);
  for (var s = 0u; s < P.ns; s = s + 1u) {
    let c = spConc(s, i);
    if (c == 0.0) { continue; }
    let m = spMass(s, i);
    var t = dv * c;
    if (s != P.tempRow) { t = min(t, m); }
    setSpMass(s, i, m - t);
    if (bc != 0u) { spAdd(spBc(1u, k, s), w * t); } else { spAdd(spCpMass(k, s), w * t); }
  }
}
// Water arriving in cell i: boundary entry k's inflow at its concentrations (bc != 0), or a
// spill from node slot n at the node's, booked as point k's spill mass.
fn spArrive(i: u32, dv: f32, w: f32, bc: u32, k: u32, n: u32) {
  spRunoff(i, -w * dv);
  for (var s = 0u; s < P.ns; s = s + 1u) {
    var c = 0.0;
    if (bc != 0u) { c = spBcConc(k, s); } else { c = spNodeConc(n, s); }
    if (c == 0.0) { continue; }
    let g = dv * c;
    setSpMass(s, i, spMass(s, i) + g);
    if (bc != 0u) { spAdd(spBc(0u, k, s), w * g); } else { spAdd(spCpGain(k, s), w * g); }
  }
}

// ---- scalar kernels (InertialKernels.hpp, KokkosSourceKernels.hpp) -----------
// Store a cell's exact volume and its closure: depth = max(V, 0)/A, eta from the cell closure.
fn setVolume(i: u32, v: vec2<u32>) {
  setCu(0u, i, v.x); setCu(1u, i, v.y);
  let vf = max(toM3(v), 0.0);
  let a = cg(0u, i);
  var d = 0.0;
  if (a > 1.0e-30) { d = vf / a; }
  setCf(0u, i, vf);
  setCf(2u, i, d);
  setCf(1u, i, cellEta(i, d));
}
fn qMagnitude(x: f32, y: f32) -> f32 { return sqrt(x * x + y * y); }
fn cflCellSpeed(qx: f32, qy: f32, h: f32) -> f32 {
  return min(qMagnitude(qx, qy) / h, P.froude * sqrt(P.g * h));
}
fn cellCflDt(lc: f32, h: f32, speed: f32) -> f32 {
  let c = sqrt(P.g * h) + speed;
  if (c > 1.0e-12) { return P.alpha * lc / c; }
  return 1.0e30;
}
fn sourceSink(rate: f32, h: f32) -> f32 {
  if (rate <= 0.0 || h <= 0.0) { return 0.0; }
  if (h >= P.dry) { return rate; }
  let t = h / P.dry;
  return rate * t * t * (3.0 - 2.0 * t);
}
// Water-only source budget of one cell over dt (sourceBudget), booked in integer units.
// `landed` is the volume after face transfers, `h` the pre-source depth. Sinks and outgoing
// coupling share one availability scale; a starved cell ends exactly empty, the few units of
// rounding residue going to its largest sink. A rounding debt below zero is carried through.
// `rkStage`: inside an RK2 stage, where every booking also books its half (decision 5); lazy
// sources between steps book in full.
fn applySources(i: u32, landed: vec2<u32>, h: f32, dt: f32, rkStage: bool) {
  let a = cg(0u, i);
  let adt = a * dt;
  let rain = max(cg(5u, i), 0.0) * adt;
  let coupling = cg(6u, i) * adt;
  let inf = sourceSink(cg(8u, i), h) * adt;
  let evap = sourceSink(cg(7u, i), h) * adt;
  if (rain == 0.0 && coupling == 0.0 && inf == 0.0 && evap == 0.0) {
    setVolume(i, landed);
    return;
  }
  var basis = landed;
  var debt = vec2<u32>(0u, 0u);
  if (isNeg64(landed)) { basis = vec2<u32>(0u, 0u); debt = landed; }
  let incoming = rain + max(coupling, 0.0);
  let available = max(toM3(basis) + incoming, 0.0);
  let out = max(-coupling, 0.0);
  let requested = inf + evap + out;
  var scale = 1.0;
  if (requested > available && requested > 0.0) { scale = available / requested; }
  if (P.ns > 0u) {
    spSources(i, rain, coupling, scale, available, inf * scale, evap * scale, adt,
              select(1.0, 0.5, rkStage));
  }
  let rainQ = quantize(rain);
  let cinQ = quantize(max(coupling, 0.0));
  let infQ = quantize(inf * scale);
  let evapQ = quantize(evap * scale);
  let outQ = quantize(out * scale);
  var infU = fromUnits(infQ);
  var evapU = fromUnits(evapQ);
  var outU = fromUnits(outQ);
  let inflow = add64(add64(basis, fromUnits(rainQ)), fromUnits(cinQ));
  var v = add64(inflow, neg64(add64(add64(infU, evapU), outU)));
  if (requested >= available || isNeg64(v)) {
    if (outQ >= infQ && outQ >= evapQ) { outU = add64(outU, v); }
    else if (infQ >= evapQ) { infU = add64(infU, v); }
    else { evapU = add64(evapU, v); }
    v = vec2<u32>(0u, 0u);
  }
  if (rkStage) {
    // RK2: the ledgers and the step's half accumulator take the halves.
    let hInf = half64(infU); let hEvap = half64(evapU); let hOut = half64(outU);
    let hCin = half64(fromUnits(cinQ)); let hRain = half64(fromUnits(rainQ));
    addCellU(2u, i, hInf);
    addCellU(4u, i, hEvap);
    addCellU(6u, i, add64(hCin, neg64(hOut)));
    addCellU(12u, i, hRain);
    addCellU(18u, i, add64(add64(hRain, hCin), neg64(add64(add64(hInf, hEvap), hOut))));
  } else {
    addCellU(2u, i, infU);
    addCellU(4u, i, evapU);
    addCellU(6u, i, add64(fromUnits(cinQ), neg64(outU)));
    addCellU(12u, i, fromUnits(rainQ)); // the rain actually applied, for the water audit
  }
  setVolume(i, add64(v, debt));
}
// Gather and clear this cell's side of every incident face accumulator.
fn gatherPending(i: u32) -> vec2<u32> {
  var pending = vec2<u32>(0u, 0u);
  var halves = vec2<u32>(0u, 0u);
  let z = vec2<u32>(0u, 0u);
  for (var p = rowBegin(i); p < rowEnd(i); p = p + 1u) {
    let code = rowCode(p);
    let e = code & 0x7fffffffu;
    if ((code & 0x80000000u) == 0u) {
      pending = add64(pending, accL(e)); setAccL(e, z);
      if (order2()) { halves = add64(halves, accHL(e)); setAccHL(e, z); }
    } else {
      pending = add64(pending, accR(e)); setAccR(e, z);
      if (order2()) { halves = add64(halves, accHR(e)); setAccHR(e, z); }
    }
  }
  if (order2()) { addCellU(18u, i, halves); }
  return pending;
}
fn perotRefresh(i: u32) {
  if ((P.flags & FLAG_PEROT) == 0u) { return; }
  var sx = 0.0; var sy = 0.0;
  let cx = cg(2u, i); let cy = cg(3u, i);
  for (var p = rowBegin(i); p < rowEnd(i); p = p + 1u) {
    let code = rowCode(p);
    let e = code & 0x7fffffffu;
    var s = 1.0; if ((code & 0x80000000u) != 0u) { s = -1.0; }
    var f = s * faceS[e] * fg(0u, e);
    if ((P.flags & FLAG_DIFFUSIVE) == 0u) { f = f * fg(8u, e); } // DW's q carries it already
    sx = sx + f * (fg(6u, e) - cx);
    sy = sy + f * (fg(7u, e) - cy);
  }
  let ia = 1.0 / cg(0u, i);
  setCf(3u, i, sx * ia);
  setCf(4u, i, sy * ia);
}
fn clockAdd(dt: f32) {
  let lo = C.tlo + dt; let c = floor(lo);
  C.thi = C.thi + c; C.tlo = lo - c;
}
fn remaining() -> f32 { return (P.span - C.thi) - C.tlo; }
fn land() { C.thi = floor(P.span); C.tlo = P.span - floor(P.span); C.mode = 2u; }
fn markSynced() { C.syncHi = C.thi; C.syncLo = C.tlo; }

// ---- one macro cycle ------------------------------------------------------------
// Sequence: plan (once per batch), then per cycle sync, haloCfl, tierAssign, faceTier, control,
// nsub x (faces, cells, boundary), advanceClock (which also plans the next cycle). Every kernel
// tests the flags the plan and control set, so a cycle after the landing is a no-op.
fn resetCounts() {
  atomicStore(&C.nActive, 0u);
  atomicStore(&C.nFaces, 0u);
  atomicStore(&C.nSeed, 0u);
  for (var k = 0u; k < 8u; k = k + 1u) {
    atomicStore(&C.cellsTier[k], 0u);
    atomicStore(&C.facesTier[k], 0u);
  }
}
fn planCycle() {
  C.doFire = 0u;
  C.doRebuild = 0u;
  C.doLazy = 0u;
  if (C.mode == 2u) { return; }
  atomicStore(&C.dtMinBits, 0x7f800000u);
  // Every kRebuildEvery cycles, or once a wetting front reached the halo's outer ring.
  let breach = (P.flags & FLAG_FRONT) != 0u && atomicLoad(&C.breach) != 0u && C.cycles > 0u;
  if (C.mode == 0u && (C.cycles >= P.rebuildEvery || breach)) {
    C.doRebuild = 1u;
    C.doLazy = 1u;
    resetCounts();
  }
}
@compute @workgroup_size(1)
fn plan() { planCycle(); }
// End of the advance, recorded after the cycles of every batch: runs only once the march has
// landed. Settle always; rebuild when due, else apply the lazy sources; then publish.
@compute @workgroup_size(1)
fn finishPlan() {
  C.doFire = 0u;
  C.doRebuild = 0u;
  C.doLazy = 0u;
  C.finishing = select(0u, 1u, C.mode == 2u && C.finished == 0u);
  if (C.finishing == 0u) { return; }
  C.doRebuild = select(0u, 1u, C.cycles >= P.rebuildEvery);
  C.doLazy = 1u;
  atomicStore(&C.dtMinBits, 0x7f800000u);
  if (C.doRebuild == 1u) { resetCounts(); }
}
// Per cell, in this order (the Kokkos settle, lazySources and seed passes, which touch only
// the cell's own data): gather pending side transfers, apply lazy sources to an inactive cell,
// then take the activation seed.
@compute @workgroup_size(WG)
fn sync(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc) { return; }
  if (C.doLazy != 0u) {
    let pending = gatherPending(i);
    // The rows settle in the same sweep, or a re-tiered cell would strand their mass.
    if (P.ns > 0u) { spSettle(i); }
    if ((P.flags & FLAG_SWE) != 0u) {
      // Settle the momentum of mass already applied, so a re-tiered or deactivated cell does
      // not pick it up stale.
      let m = gatherMomentum(i);
      if (m.x != 0.0 || m.y != 0.0) {
        let ia = 1.0 / cg(0u, i);
        setCf(3u, i, cf(3u, i) + m.x * ia); setCf(4u, i, cf(4u, i) + m.y * ia);
      }
    }
    if (!isZero64(pending)) { setVolume(i, add64(vol64(i), pending)); }
    let dtl = (C.thi - C.syncHi) + (C.tlo - C.syncLo);
    if (cu(8u, i) == 0u && dtl > 0.0) { applySources(i, vol64(i), cf(2u, i), dtl, false); }
  }
  if (C.doRebuild != 0u) {
    var thresh = P.hOn; if (cu(8u, i) != 0u) { thresh = P.hOff; }
    // Momentum physics can accelerate any wet neighbor: every cell is active.
    let s = cf(2u, i) >= thresh || cg(6u, i) != 0.0 || cu(10u, i) != 0u || (P.flags & FLAG_PHYSICS) != 0u;
    setCu(9u, i, select(0u, 1u, s));
    if (s && (P.flags & FLAG_FRONT) != 0u) { atomicAdd(&C.nSeed, 1u); }
  }
}
// Per cell, at a rebuild: active when seeded or beside a seed.
fn neighbor(i: u32, p: u32) -> u32 {
  let e = rowCode(p) & 0x7fffffffu;
  var j = cL(e); if (j == i) { j = cR(e); }
  return j;
}
@compute @workgroup_size(WG)
fn halo(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc || C.doRebuild == 0u) { return; }
  var on = cu(9u, i);
  for (var p = rowBegin(i); p < rowEnd(i) && on == 0u; p = p + 1u) { on = cu(9u, neighbor(i, p)); }
  setCu(8u, i, on);
}
// FRONT_REBUILD at a rebuild: a dry-bed front can cross several rings between two macro cycles,
// so the halo widens by Jacobi rings (S.y 1: copy the active flags into the seeds; 2: activate
// the cells beside a seed), unless nothing or everything is seeded.
@compute @workgroup_size(WG)
fn ring(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc || C.doRebuild == 0u || (P.flags & FLAG_FRONT) == 0u) { return; }
  let n = atomicLoad(&C.nSeed);
  if (n == 0u || n == P.nc) { return; }
  if (S.y == 1u) { setCu(9u, i, cu(8u, i)); return; }
  if (cu(9u, i) != 0u) { return; }
  for (var p = rowBegin(i); p < rowEnd(i); p = p + 1u) {
    if (cu(9u, neighbor(i, p)) != 0u) { setCu(8u, i, 1u); return; }
  }
}
// FRONT_REBUILD at a rebuild: the halo's outer ring (active cells beside an inactive one), whose
// wetting triggers the next rebuild.
@compute @workgroup_size(WG)
fn frontier(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc || C.doRebuild == 0u || (P.flags & FLAG_FRONT) == 0u) { return; }
  var f = 0u;
  if (cu(8u, i) != 0u) {
    for (var p = rowBegin(i); p < rowEnd(i) && f == 0u; p = p + 1u) {
      f = select(1u, 0u, cu(8u, neighbor(i, p)) != 0u);
    }
  }
  setCu(14u, i, f);
}
// DIFFUSIVE_WAVE: the steepest free-surface slope over a cell's faces (refreshDiffusiveSlopes).
fn steepestSlope(i: u32) -> f32 {
  var smax = 0.0;
  for (var p = rowBegin(i); p < rowEnd(i); p = p + 1u) {
    let e = rowCode(p) & 0x7fffffffu;
    smax = max(smax, abs(((cf(1u, cR(e)) - cf(1u, cL(e))) + fg(10u, e)) * fg(2u, e)));
  }
  return smax;
}
// A wet cell's stable step under the marcher's scheme (the Kokkos cfl_min / cfl_refresh).
fn cellStableDt(i: u32, h: f32) -> f32 {
  if ((P.flags & FLAG_DIFFUSIVE) != 0u) {
    // The explicit-diffusion bound: alpha L^2 n sqrt(S) / (4 h^(5/3)).
    let lc = cg(4u, i);
    let sEps = P.dhEps / max(lc, 1.0e-9);
    let h53 = h * pow(h * h, 1.0 / 3.0);
    return P.alpha * lc * lc * cg(18u, i) * sqrt(max(steepestSlope(i), sEps)) / (4.0 * h53);
  }
  var speed = 0.0;
  if ((P.flags & FLAG_SWE) != 0u) {
    // Cell momentum over depth, against the positivity length (kLchar under FULL_SWE).
    if (h > 1.0e-6) { speed = qMagnitude(cf(3u, i), cf(4u, i)) / h; }
  } else if ((P.flags & FLAG_PEROT) != 0u && h > 1.0e-6) {
    speed = cflCellSpeed(cf(3u, i), cf(4u, i), h);
  }
  return cellCflDt(cg(4u, i), h, speed);
}
// Per cell: the CFL bound and its exact minimum. A rebuild counts the active cells and takes the
// minimum over every active cell (dry ones at DT_MAX); between rebuilds the minimum over active
// wet cells may only tighten dt0 (refreshDt0). The tail keeps its dt.
@compute @workgroup_size(WG)
fn cfl(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc || cu(8u, i) == 0u) { return; }
  if (C.doRebuild != 0u) { atomicAdd(&C.nActive, 1u); }
  if (C.doRebuild == 0u && (C.mode != 0u || C.doLazy != 0u || C.finishing != 0u)) { return; }
  let h = cf(2u, i);
  var dt = 1.0e30;
  if (h > P.dry) {
    dt = cellStableDt(i, h);
  } else if (C.doRebuild == 0u) {
    return;
  }
  if (C.doRebuild == 1u) {
    dt = min(dt, P.dtMax);
    setCf(5u, i, dt); // the tier assignment's dt_cell
  } else {
    dt = dt / f32(1u << cu(11u, i)); // tighten-only refresh: the cell fires every 2^tier dt0
  }
  atomicMin(&C.dtMinBits, bitcast<u32>(dt));
}
// Tiers of the active cells (rebuild only; an inactive cell keeps its stale tier, as on the CPU):
// floor(log2(dt_cell / dt0)), capped at K - 1; coupling and pinned cells stay on tier 0.
@compute @workgroup_size(WG)
fn tierAssign(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc || C.doRebuild == 0u || cu(8u, i) == 0u) { return; }
  var tk = 0u;
  if (P.tiers > 1u) {
    var d0 = bitcast<f32>(atomicLoad(&C.dtMinBits));
    if (d0 >= 1.0e30) { d0 = P.dtMax; }
    let ratio = cf(5u, i) / d0;
    if (ratio >= 2.0) { tk = min(u32(frexp(ratio).exp - 1), P.tiers - 1u); }
    if (cg(6u, i) != 0.0 || cu(10u, i) != 0u) { tk = 0u; }
  }
  setCu(11u, i, tk);
  if ((P.flags & FLAG_FRONT) == 0u || P.tiers == 1u) { atomicAdd(&C.cellsTier[tk], 1u); }
}
// FRONT_REBUILD with LTS at a rebuild: dry halo cells got the coarsest tier, so a front could
// enter them only once per macro cycle. Each dry active cell takes the finest tier among its
// active neighbors, repeated to the fixed point (the CPU's Gauss-Seidel sweep in edge order
// reaches the same fixed point whenever it converges within its five passes). Jacobi rounds
// alternate between kTier and kTierNext (S.y 1: kTier to kTierNext; 2: back), then tierCount.
@compute @workgroup_size(WG)
fn pull(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc || C.doRebuild == 0u) { return; }
  var src = 11u; var dst = 15u;
  if (S.y == 2u) { src = 15u; dst = 11u; }
  var t = cu(src, i);
  if (cu(8u, i) != 0u && cf(2u, i) <= P.dry) {
    for (var p = rowBegin(i); p < rowEnd(i); p = p + 1u) {
      let j = neighbor(i, p);
      if (cu(8u, j) != 0u) { t = min(t, cu(src, j)); }
    }
  }
  setCu(dst, i, t);
}
@compute @workgroup_size(WG)
fn tierCount(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc || C.doRebuild == 0u || cu(8u, i) == 0u) { return; }
  atomicAdd(&C.cellsTier[cu(11u, i)], 1u);
}
@compute @workgroup_size(WG)
fn faceTier(@builtin(global_invocation_id) id: vec3<u32>) {
  let e = id.x; if (e >= P.ne || C.doRebuild == 0u) { return; }
  let a = cL(e); let b = cR(e);
  if (cu(8u, a) != 0u && cu(8u, b) != 0u) {
    let kf = min(cu(11u, a), cu(11u, b));
    setFtier(e, kf);
    atomicAdd(&C.nFaces, 1u);
    atomicAdd(&C.facesTier[kf], 1u);
  } else {
    setFtier(e, P.tiers);  // sentinel: not marched
    faceS[e] = 0.0;
  }
}
fn adoptRebuild() {
  atomicStore(&C.breach, 0u);
  var d0 = bitcast<f32>(atomicLoad(&C.dtMinBits));
  if (d0 >= 1.0e30) { d0 = P.dtMax; }
  C.dt0 = d0;
  C.cycles = 0u;
  C.nActiveLast = atomicLoad(&C.nActive);
  C.nFacesLast = atomicLoad(&C.nFaces);
  for (var k = 0u; k < 8u; k = k + 1u) {
    C.facesTierLast[k] = atomicLoad(&C.facesTier[k]);
    C.occupancy[k] = C.occupancy[k] + atomicLoad(&C.cellsTier[k]);
  }
  markSynced();
}
@compute @workgroup_size(1)
fn control() {
  if (C.mode == 2u) { return; }
  if (C.doRebuild == 1u) {
    adoptRebuild();
  } else if (C.mode == 0u) {
    let fresh = bitcast<f32>(atomicLoad(&C.dtMinBits));
    if (fresh < C.dt0) { C.dt0 = fresh; }
  }
  if (C.mode == 1u) { C.dt = C.dtTail; C.doFire = 1u; return; }
  let rem = remaining();
  if (rem <= 0.0) { land(); return; }
  if (C.nActiveLast == 0u) {
    C.lastDt = rem;
    C.cycles = P.rebuildEvery;
    land();
    return;
  }
  // A full macro cycle of nsub base steps, or the tail: the WHOLE remaining span in equal global
  // steps (each in (dt0/2, dt0]), with every marched face and active cell firing each step on
  // the frozen tiers; rem < nsub dt0 makes it at most nsub steps, one recorded cycle.
  if (C.dt0 * f32(P.nsub) <= rem) { C.dt = C.dt0; C.doFire = 1u; return; }
  if (rem <= C.dt0 * 1.0e-9) { land(); return; } // sub-ulp residue of the landing: no physics
  let nt = max(1u, u32(ceil(rem / C.dt0)));
  C.tailLeft = nt;
  C.dtTail = rem / f32(nt);
  C.mode = 1u;
  C.dt = C.dtTail;
  C.doFire = 1u;
}
@compute @workgroup_size(1)
fn finishControl() {
  if (C.finishing == 0u) { return; }
  if (C.doRebuild == 1u) { adoptRebuild(); } else { markSynced(); }
  C.finished = 1u;
}
// The volume a face may take from its exporter in one firing (the positivity share): the face
// fires 2^(tier(exporter) - kf) times per firing of the exporter, and each firing may spend that
// fraction of exchange_beta over the exporter's face count (a tail step is one global firing).
fn faceBudget(ex: u32, kf: u32) -> f32 {
  var refire = 1.0;
  if (C.mode == 0u) { refire = f32(1u << (cu(11u, ex) - kf)); }
  var share = P.beta3;
  if (isQuad(ex)) { share = P.beta3 * 0.75; }
  return share / refire * cf(0u, ex);
}
// Book an exact integer transfer of `m3` (positive cL to cR) into the two face-side accumulators.
fn bookTransfer(e: u32, m3: f32) {
  let dM = fromUnits(quantize(m3));
  setAccL(e, add64(accL(e), neg64(dM)));
  setAccR(e, add64(accR(e), dM));
  if (order2()) {
    let h = half64(dM);
    setAccHL(e, add64(accHL(e), neg64(h)));
    setAccHR(e, add64(accHR(e), h));
  }
}
// DIFFUSIVE_WAVE face (fireFacesDiffusive): Manning quasi-steady discharge with the regularized
// slope, already scaled by the face conveyance.
fn diffusiveFace(e: u32, dt: f32, kf: u32) {
  let a = cL(e); let b = cR(e);
  let hf = faceFlowDepth(e, cf(1u, a), cf(1u, b));
  if (hf <= P.dry) { faceS[e] = 0.0; return; }
  var deta = (cf(1u, b) - cf(1u, a)) + fg(10u, e);
  if (abs(deta) < ETA_DEADBAND) { deta = 0.0; }
  let slope = deta * fg(2u, e);
  var q = 0.0;
  let nf = fg(3u, e); // the face's Manning n under DIFFUSIVE_WAVE
  if (hf > 0.0 && nf > 0.0 && slope != 0.0) {
    let sEps = P.dhEps * fg(2u, e);
    q = -hf * pow(hf * hf, 1.0 / 3.0) * slope / (nf * sqrt(max(abs(slope), sEps)));
  }
  q = q * fg(8u, e);
  var ex = b; if (q > 0.0) { ex = a; }
  let budget = faceBudget(ex, kf);
  let take = abs(q) * fg(0u, e) * dt;
  if (take > budget) { if (take > 0.0) { q = q * (budget / take); } else { q = 0.0; } }
  faceS[e] = q;
  bookTransfer(e, q * fg(0u, e) * dt);
  if (P.ns > 0u) { spFace(e, quantize(q * fg(0u, e) * dt) * M3_PER_UNIT, hf, dt, kf, 1.0); }
}

// ---- FULL_SWE (SweKernels.hpp) ------------------------------------------------
// One side of a face in its normal frame after hydrostatic reconstruction.
struct Side { h: f32, un: f32, ut: f32 };
// The HLLC flux with the hydrostatic pressure of the LEFT reconstructed state taken out of the
// normal component: fnL = fn - g/2 L.h^2 (W5 notes, decision 2). dh = R.h - L.h, formed by the
// caller from the local-head difference when both sides are wet, so a lake at rest gives exactly
// zero. fh is the mass flux, ft the tangential momentum flux.
struct Flux { fh: f32, fnL: f32, ft: f32 };
fn hllc(L: Side, R: Side, dh: f32) -> Flux {
  var o = Flux(0.0, 0.0, 0.0);
  if (L.h <= 0.0 && R.h <= 0.0) { return o; }
  let cl = sqrt(P.g * L.h); let cr = sqrt(P.g * R.h);
  var sl = 0.0; var sr = 0.0;
  if (L.h > 0.0 && R.h > 0.0) {
    let ustar = 0.5 * (L.un + R.un) + cl - cr;
    let cstar = 0.5 * (cl + cr) + 0.25 * (L.un - R.un);
    sl = min(L.un - cl, ustar - cstar);
    sr = max(R.un + cr, ustar + cstar);
  } else if (L.h > 0.0) {
    sl = L.un - cl; sr = L.un + 2.0 * cl;
  } else {
    sl = R.un - 2.0 * cr; sr = R.un + cr;
  }
  let dl = L.h * (L.un - sl); let dr = R.h * (R.un - sr);
  let den = dr - dl;
  var sstar = 0.5 * (L.un + R.un);
  if (abs(den) > 1.0e-14) { sstar = (sl * dr - sr * dl) / den; }
  let dp = 0.5 * P.g * dh * (R.h + L.h); // g/2 (R.h^2 - L.h^2)
  let fhl = L.h * L.un; let fhr = R.h * R.un;
  if (sl >= 0.0) {
    o.fh = fhl; o.fnL = fhl * L.un; o.ft = fhl * L.ut;
  } else if (sr <= 0.0) {
    o.fh = fhr; o.fnL = fhr * R.un + dp; o.ft = fhr * R.ut;
  } else {
    let inv = 1.0 / (sr - sl);
    o.fh = (sr * fhl - sl * fhr + sl * sr * dh) * inv;
    o.fnL = (sr * fhl * L.un - sl * fhr * R.un + sl * sr * (fhr - fhl) - sl * dp) * inv;
    o.ft = o.fh * select(R.ut, L.ut, sstar >= 0.0);
  }
  return o;
}
// FULL_SWE's dispersive depth: the higher cell surface over the centroid crest zface, at least
// zero (the crest offsets are to the VFR_FACE crest; spAcc's zface field closes the gap).
fn spSweDepth(e: u32) -> f32 {
  let s = max(cf(1u, cL(e)) + fg(1u, e), cf(1u, cR(e)) + fg(9u, e));
  return max(s + faceS[P.offSpAcc + 2u * P.ns * P.ne + e], 0.0);
}
fn momL(e: u32) -> vec2<f32> { return vec2<f32>(faceS[P.offMom + e], faceS[P.offMom + P.ne + e]); }
fn momR(e: u32) -> vec2<f32> {
  return vec2<f32>(faceS[P.offMom + 2u * P.ne + e], faceS[P.offMom + 3u * P.ne + e]);
}
// RECONSTRUCTION_ORDER 2: a limited gradient (fields f, f+1 of cellF) dotted with an arm.
fn gradAt(f: u32, i: u32, arm: vec2<f32>) -> f32 { return cf(f, i) * arm.x + cf(f + 1u, i) * arm.y; }
fn addMomentum(e: u32, bookL: vec2<f32>, bookR: vec2<f32>) {
  let mL = momL(e) + bookL; let mR = momR(e) + bookR;
  faceS[P.offMom + e] = mL.x; faceS[P.offMom + P.ne + e] = mL.y;
  faceS[P.offMom + 2u * P.ne + e] = mR.x; faceS[P.offMom + 3u * P.ne + e] = mR.y;
}
// Interior FULL_SWE face (fireFacesSwe): swe::faceFlux at first order, swe::faceFluxReconBed
// with the MUSCL extrapolation (limited eta, depth and velocity gradients) at second order.
// Surfaces are taken relative to the face's crest bed; z = eta - h is the flat-equivalent bed.
// The mass transfer is an exact integer; each side books its momentum (m^4/s) without its own
// cell's hydrostatic pressure, which sums to zero around the cell. The second-order bed source
// -g/2 (h + hf) * (eta extrapolation) is formed from the extrapolation itself (no cancellation).
fn sweFace(e: u32, dt: f32, kf: u32) {
  let a = cL(e); let b = cR(e);
  let hA = cf(2u, a); let hB = cf(2u, b);
  let nx = fg(4u, e); let ny = fg(5u, e);
  var uA = vec2<f32>(0.0, 0.0); var uB = vec2<f32>(0.0, 0.0);
  if (hA > P.dry) { uA = vec2<f32>(cf(3u, a), cf(4u, a)) / hA; }
  if (hB > P.dry) { uB = vec2<f32>(cf(3u, b), cf(4u, b)) / hB; }
  var dEtaL = 0.0; var dEtaR = 0.0; var hLf = hA; var hRf = hB;
  if (order2()) {
    let armA = vec2<f32>(fg(6u, e) - cg(2u, a), fg(7u, e) - cg(3u, a));
    let armB = vec2<f32>(fg(6u, e) - cg(2u, b), fg(7u, e) - cg(3u, b));
    dEtaL = gradAt(6u, a, armA); dEtaR = gradAt(6u, b, armB);
    hLf = max(hA + gradAt(8u, a, armA), 0.0); hRf = max(hB + gradAt(8u, b, armB), 0.0);
    uA = uA + vec2<f32>(gradAt(10u, a, armA), gradAt(12u, a, armA));
    uB = uB + vec2<f32>(gradAt(10u, b, armB), gradAt(12u, b, armB));
  }
  let etaL = cf(1u, a) + fg(1u, e) + dEtaL; let etaR = cf(1u, b) + fg(9u, e) + dEtaR;
  let zf = max(etaL - hLf, etaR - hRf);
  let Lh = max(etaL - zf, 0.0); let Rh = max(etaR - zf, 0.0);
  let n = vec2<f32>(nx, ny); let t = vec2<f32>(-ny, nx);
  let xdt = fg(0u, e) * dt;
  let srcL = -0.5 * P.g * (hA + hLf) * dEtaL * n;
  let srcR = 0.5 * P.g * (hB + hRf) * dEtaR * n;
  if (Lh <= 0.0 && Rh <= 0.0) {
    // No flux; at second order the bed source still acts on a wet side. Row dispersion still
    // gets its call.
    faceS[e] = 0.0;
    if (dEtaL != 0.0 || dEtaR != 0.0) { addMomentum(e, srcL * xdt, srcR * xdt); }
    if (P.ns > 0u) { spFace(e, 0.0, spSweDepth(e), dt, kf, 1.0); }
    return;
  }
  var dh = Rh - Lh;
  if (Lh > 0.0 && Rh > 0.0) { dh = ((cf(1u, b) - cf(1u, a)) + fg(10u, e)) + (dEtaR - dEtaL); }
  var L = Side(Lh, 0.0, 0.0); var R = Side(Rh, 0.0, 0.0);
  if (hA > P.dry && Lh > 0.0) { L.un = dot(uA, n); L.ut = dot(uA, t); }
  if (hB > P.dry && Rh > 0.0) { R.un = dot(uB, n); R.ut = dot(uB, t); }
  let F = hllc(L, R, dh);
  // Conveyance scales the whole flux; the positivity share scales mass and momentum together.
  var c = fg(8u, e);
  let fh = F.fh * c;
  if (fh != 0.0) {
    var ex = b; if (fh > 0.0) { ex = a; }
    let budget = faceBudget(ex, kf);
    let take = abs(fh) * fg(0u, e) * dt;
    if (take > budget) { if (take > 0.0) { c = c * (budget / take); } else { c = 0.0; } }
  }
  faceS[e] = F.fh * c;
  bookTransfer(e, F.fh * c * xdt);
  if (P.ns > 0u) { spFace(e, quantize(F.fh * c * xdt) * M3_PER_UNIT, spSweDepth(e), dt, kf, 1.0); }
  // Left: -c (fnL n + ft t) + (1 - c) g/2 L.h^2 n; right: c (fnR n + ft t) - (1 - c) g/2 R.h^2 n,
  // with fnR = fnL - g/2 (R.h^2 - L.h^2); plus the bed sources.
  let fnR = F.fnL - 0.5 * P.g * dh * (Rh + Lh);
  let bookL = (-c * (F.fnL * n + F.ft * t) + (1.0 - c) * 0.5 * P.g * Lh * Lh * n + srcL) * xdt;
  let bookR = (c * (fnR * n + F.ft * t) - (1.0 - c) * 0.5 * P.g * Rh * Rh * n + srcR) * xdt;
  addMomentum(e, bookL, bookR);
}
// Gather and clear this cell's side of every incident face's momentum accumulators.
fn gatherMomentum(i: u32) -> vec2<f32> {
  var m = vec2<f32>(0.0, 0.0);
  for (var p = rowBegin(i); p < rowEnd(i); p = p + 1u) {
    let code = rowCode(p);
    let e = code & 0x7fffffffu;
    if ((code & 0x80000000u) == 0u) {
      m = m + momL(e); faceS[P.offMom + e] = 0.0; faceS[P.offMom + P.ne + e] = 0.0;
    } else {
      m = m + momR(e); faceS[P.offMom + 2u * P.ne + e] = 0.0; faceS[P.offMom + 3u * P.ne + e] = 0.0;
    }
  }
  return m;
}
@compute @workgroup_size(WG)
fn faces(@builtin(global_invocation_id) id: vec3<u32>) {
  let e = id.x; if (e >= P.ne || C.doFire == 0u) { return; }
  let kf = ftier(e);
  if (kf >= P.tiers) { return; }
  var dt = C.dtTail;
  if (C.mode == 1u) {
    if (S.x >= C.tailLeft) { return; }
  } else {
    if ((S.x & ((1u << kf) - 1u)) != 0u) { return; }
    dt = C.dt0 * f32(1u << kf);
  }
  if ((P.flags & FLAG_SWE) != 0u) { sweFace(e, dt, kf); return; }
  if ((P.flags & FLAG_DIFFUSIVE) != 0u) { diffusiveFace(e, dt, kf); return; }
  let conv = fg(8u, e);
  if (conv == 0.0) { faceS[e] = 0.0; return; }
  let width = fg(0u, e) * conv;
  let a = cL(e); let b = cR(e);
  let headA = cf(1u, a); let headB = cf(1u, b);
  let hf = faceFlowDepth(e, headA, headB);
  if (hf <= P.dry) { faceS[e] = 0.0; return; }
  let q0 = faceS[e];
  var qhat = q0;
  var qMag = abs(q0);
  if ((P.flags & FLAG_PEROT) != 0u) {
    let qfx = 0.5 * (cf(3u, a) + cf(3u, b));
    let qfy = 0.5 * (cf(4u, a) + cf(4u, b));
    let qn = qfx * fg(4u, e) + qfy * fg(5u, e);
    if ((P.flags & FLAG_TIME_BLEND) != 0u) {
      var blend = 0.0;
      if (dt > 0.0 && P.theta != 1.0) { blend = 1.0 - exp(log(P.theta) * (dt / P.thetaRef)); }
      qhat = q0 + blend * (qn - q0);
    } else {
      qhat = P.theta * q0 + (1.0 - P.theta) * qn;
    }
    qMag = max(qMag, qMagnitude(qfx, qfy));
  }
  var deta = (headB - headA) + fg(10u, e); // local heads plus the reference-bed step
  if (abs(deta) < ETA_DEADBAND) { deta = 0.0; }
  let slope = deta * fg(2u, e);
  var adv = 0.0;
  if ((P.flags & (FLAG_ADVECTION | FLAG_PEROT)) == (FLAG_ADVECTION | FLAG_PEROT)) {
    let hL = cf(2u, a); let hR = cf(2u, b);
    if (hL > P.dry && hR > P.dry) {
      let unL = (cf(3u, a) * fg(4u, e) + cf(4u, a) * fg(5u, e)) / hL;
      let unR = (cf(3u, b) * fg(4u, e) + cf(4u, b) * fg(5u, e)) / hR;
      var fl = q0; if (unL > 0.0) { fl = hL * unL; }
      var fr = hR * unR; if (unR > 0.0) { fr = q0; }
      adv = (unR * fr - unL * fl) * fg(2u, e);
    }
  }
  let h73 = hf * hf * pow(hf, 1.0 / 3.0);
  var qn1 = (qhat - dt * (P.g * hf * slope + adv)) / (1.0 + P.g * dt * fg(3u, e) * qMag / h73);
  let qcap = P.froude * hf * sqrt(P.g * hf);
  qn1 = clamp(qn1, -qcap, qcap);
  var ex = b; if (qn1 > 0.0) { ex = a; }
  let budget = faceBudget(ex, kf);
  let take = abs(qn1) * width * dt;
  if (take > budget) { if (take > 0.0) { qn1 = qn1 * (budget / take); } else { qn1 = 0.0; } }
  faceS[e] = qn1;
  let dq = quantize(qn1 * width * dt);
  let dM = fromUnits(dq); // positive: cL → cR
  setAccL(e, add64(accL(e), neg64(dM)));
  setAccR(e, add64(accR(e), dM));
  if (P.ns > 0u) { spFace(e, dq * M3_PER_UNIT, hf, dt, kf, conv); }
}
// FULL_SWE momentum of a cell that just fired (fireCells): M = A q + the face bookings + the wall
// pressure fluxes at the beginning state, then semi-implicit friction against the new depth.
// Face and wall bookings leave out the cell's own hydrostatic pressure g/2 h0^2 (it sums to zero
// around the cell); wherever the FP64 marchers' sum at this point does not close, the missing
// part is added back: faces that were not marched (an inactive neighbor), the walls of a cell
// too dry for them, and the boundary edges, which the FP64 marchers book after the cell update.
fn sweMomentum(i: u32, h0: f32, mFaces: vec2<f32>, dt: f32) {
  var m = mFaces;
  let qx0 = cf(3u, i); let qy0 = cf(4u, i);
  var open = vec2<f32>(0.0, 0.0); // sum of n_out L over the edges whose pressure is not booked
  for (var p = rowBegin(i); p < rowEnd(i); p = p + 1u) {
    let code = rowCode(p);
    let e = code & 0x7fffffffu;
    if (ftier(e) >= P.tiers) {
      var sgn = 1.0; if ((code & 0x80000000u) != 0u) { sgn = -1.0; }
      open = open + sgn * fg(0u, e) * vec2<f32>(fg(4u, e), fg(5u, e));
    }
  }
  let w0 = idx[P.offWallPtr + i]; let w1 = idx[P.offWallPtr + i + 1u];
  for (var w = w0; w < w1; w = w + 1u) {
    let n = vec2<f32>(faceG[P.offWallG + w], faceG[P.offWallG + P.nw + w]);
    let len = faceG[P.offWallG + 2u * P.nw + w];
    if (h0 > P.dry) {
      // Mirror ghost: the same depth, the normal velocity reversed (MUSCL-extrapolated to the
      // wall at second order, with its bed source).
      var u = vec2<f32>(qx0, qy0) / h0;
      var hf = h0; var dEta = 0.0;
      if (order2()) {
        let arm = vec2<f32>(faceG[P.offWallG + 3u * P.nw + w] - cg(2u, i),
                            faceG[P.offWallG + 4u * P.nw + w] - cg(3u, i));
        dEta = gradAt(6u, i, arm);
        hf = max(h0 + gradAt(8u, i, arm), 0.0);
        u = u + vec2<f32>(gradAt(10u, i, arm), gradAt(12u, i, arm));
      }
      let t = vec2<f32>(-n.y, n.x);
      let un = dot(u, n); let ut = dot(u, t);
      let F = hllc(Side(hf, un, ut), Side(hf, -un, ut), 0.0);
      m = m + (-(F.fnL * n + F.ft * t) - 0.5 * P.g * (h0 + hf) * dEta * n) * (len * dt);
    } else {
      open = open + len * n;
    }
  }
  // The boundary edges' share (kSbcX/Y: sum of n L over them) joins the cell's momentum now,
  // as on the FP64 marchers, whose boundary pass then books its full pressure flux against the
  // updated cell; the boundary's Riemann solve sees the same intermediate momentum.
  if ((P.flags & FLAG_SWE) != 0u) { open = open + vec2<f32>(cg(19u, i), cg(20u, i)); }
  m = m + 0.5 * P.g * h0 * h0 * dt * open;
  let h = cf(2u, i);
  if (h <= P.dry) { setCf(3u, i, 0.0); setCf(4u, i, 0.0); return; }
  let A = cg(0u, i);
  var qx = (A * qx0 + m.x) / A; var qy = (A * qy0 + m.y) / A;
  let qm = sqrt(qx * qx + qy * qy);
  if (qm != 0.0) {
    let n = cg(18u, i);
    let den = 1.0 + P.g * dt * n * n * qm / (h * h * pow(h, 1.0 / 3.0));
    qx = qx / den; qy = qy / den;
  }
  setCf(3u, i, qx); setCf(4u, i, qy);
}
@compute @workgroup_size(WG)
fn cells(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc || C.doFire == 0u || cu(8u, i) == 0u) { return; }
  var dt = C.dtTail;
  if (C.mode == 1u) {
    // The tail fires every cell as one global tier.
    if (S.x >= C.tailLeft || S.y == 2u) { return; }
  } else {
    let k = cu(11u, i);
    if (((S.x + 1u) & ((1u << k) - 1u)) != 0u) { return; }
    if ((S.y == 1u && k != 0u) || (S.y == 2u && k == 0u)) { return; }
    dt = C.dt0 * f32(1u << k);
  }
  let h0 = cf(2u, i);
  if ((P.flags & FLAG_PHYSICS) != 0u) {
    // Momentum physics: boundary edges read the state held at the start of this firing.
    setPhf(10u, i, h0); setPhf(11u, i, cf(1u, i)); setPhf(12u, i, cf(3u, i)); setPhf(13u, i, cf(4u, i));
  }
  let pending = gatherPending(i);
  let landed = add64(vol64(i), pending);
  // Rows land with the water, then mix the accepted arrivals and pay the sinks (applySources).
  if (P.ns > 0u) { spLand(i, toM3(pending), select(1.0, 0.5, order2())); }
  var m = vec2<f32>(0.0, 0.0);
  if ((P.flags & FLAG_SWE) != 0u) { m = gatherMomentum(i); }
  applySources(i, landed, h0, dt, order2());
  if ((P.flags & FLAG_FRONT) != 0u && cu(14u, i) != 0u && cf(2u, i) >= P.hOn) {
    atomicStore(&C.breach, 1u); // the wetting front reached the halo's outer ring
  }
  if ((P.flags & FLAG_SWE) != 0u) { sweMomentum(i, h0, m, dt); return; }
  perotRefresh(i);
}
// Signed comparisons of 64-bit integers (|a - b| < 2^63).
fn lt64(a: vec2<u32>, b: vec2<u32>) -> bool { return isNeg64(add64(a, neg64(b))); }
fn min64(a: vec2<u32>, b: vec2<u32>) -> vec2<u32> { return select(b, a, lt64(a, b)); }
fn max64(a: vec2<u32>, b: vec2<u32>) -> vec2<u32> { return select(a, b, lt64(a, b)); }
// The mean depth of cell i under the local free surface eta (inertial::cellVolumeFromEta / A).
fn cellDepthAt(i: u32, eta: f32) -> f32 {
  if ((P.flags & FLAG_VFR) == 0u) { return max(eta, 0.0); }
  if (isQuad(i)) { return quadH(i, eta, P.mwf); }
  return vfrH(cg(9u, i), cg(10u, i), cg(11u, i), eta, P.mwf);
}
// Non-WALL boundary edges after the tier-0 cells fired (== the Kokkos fireBc and the CPU
// marcher, LOCAL_INERTIAL): one thread per run keeps a cell's edges in their serial order.
// NORMAL_FLOW conveys at the Manning normal depth; SPECIFIED_FLOW and RATING_CURVE impose the
// discharge the router resolved for this advance; SPECIFIED_STAGE integrates the interior
// momentum law against a ghost held at the prescribed stage, then clamps the cell to move at
// most to that stage in one step. A type set to WALL at run time moves no water. The applied
// volume is an exact integer; an outflow takes at most the water the cell holds.
fn bcS(f: u32, k: u32) -> f32 { return faceS[P.offBcS + f * P.nbc + k]; }
fn bcG(f: u32, k: u32) -> f32 { return faceG[P.offBcG + f * P.nbc + k]; }
fn setBcS(f: u32, k: u32, v: f32) { faceS[P.offBcS + f * P.nbc + k] = v; }
fn addBcU(f: u32, k: u32, v: vec2<u32>) {
  let at = P.offBcU + f * P.nbc + k;
  let r = add64(vec2<u32>(faceU[at], faceU[at + P.nbc]), v);
  faceU[at] = r.x; faceU[at + P.nbc] = r.y;
}
fn bcPerot(i: u32, k: u32, outward: f32) {
  let ia = 1.0 / cg(0u, i);
  setCf(3u, i, cf(3u, i) + outward * (faceG[P.offBcG + 2u * P.nbc + k] - cg(2u, i)) * ia);
  setCf(4u, i, cf(4u, i) + outward * (faceG[P.offBcG + 3u * P.nbc + k] - cg(3u, i)) * ia);
}
// swe::depthFromInvariantAndDischarge: the depth h = s^2 with 2 sqrt(g) s^3 - r s^2 + q = 0
// (outgoing invariant r = u + 2c and the prescribed discharge q), else the interior depth.
fn depthFromInvariant(r: f32, q: f32, hInt: f32) -> f32 {
  let rg = sqrt(P.g);
  var s = sqrt(select(1.0e-6, hInt, hInt > 0.0));
  for (var it = 0; it < 40; it = it + 1) {
    let f = 2.0 * rg * s * s * s - r * s * s + q;
    let df = 6.0 * rg * s * s - 2.0 * r * s;
    if (!(abs(df) > 1.0e-12)) { break; }
    let s1 = s - f / df;
    if (!(s1 > 0.0)) { break; }
    let step = abs(s1 - s);
    s = s1;
    if (step < 2.0e-7 * (1.0 + s)) { break; }
  }
  let h = s * s;
  if (h > 0.0 && abs(2.0 * rg * s * s * s - r * s * s + q) < 1.0e-5 * (1.0 + abs(q))) { return h; }
  return hInt;
}
// FULL_SWE boundary edge (swe::boundaryFlux): a ghost state per type, the HLLC flux between the
// interior and the ghost (no reconstruction: both stand on the cell's bed), mass prescribed for
// NORMAL_FLOW, SPECIFIED_FLOW and RATING_CURVE. Returns (inflow m3/s, dqx, dqy), the momentum the
// cell takes (m^2/s): the full flux -c F + the hydrostatic correction (zero here) as the FP64
// marchers book it, the cell update having already taken the matching g/2 h0^2 share.
fn sweBoundary(i: u32, k: u32, ty: u32, L: f32, nMan: f32, dt: f32) -> vec3<f32> {
  var hi = cf(2u, i); var etai = cf(1u, i); var qi = vec2<f32>(cf(3u, i), cf(4u, i));
  if ((P.flags & FLAG_PHYSICS) != 0u) { hi = phf(10u, i); etai = phf(11u, i); qi = vec2<f32>(phf(12u, i), phf(13u, i)); }
  let zi = etai - hi; // the flat-equivalent bed, local
  let nrm = vec2<f32>(bcG(11u, k), bcG(12u, k));
  var un = 0.0; var ut = 0.0;
  if (hi > P.dry) {
    let ux = qi.x / hi; let uy = qi.y / hi;
    un = ux * nrm.x + uy * nrm.y; ut = -ux * nrm.y + uy * nrm.x;
  }
  var hg = hi; var ung = un; var utg = ut;
  var prescribed = false; var qOut = 0.0; // outward per metre
  if (ty == 0u) {
    ung = -un;
  } else if (ty == 1u) {
    let S = bcG(4u, k);
    prescribed = true;
    if (S != 0.0 && hi > 0.0 && nMan > 0.0) {
      qOut = sign(S) * hi * pow(hi * hi, 1.0 / 3.0) * sqrt(abs(S)) / nMan;
      ung = qOut / hi;
    } else {
      ung = -un;
    }
  } else if (ty == 2u) {
    let ci = sqrt(P.g * max(hi, 0.0));
    if (!(hi > P.dry && un >= ci)) { // else supercritical outflow: transmissive
      hg = max(bcG(8u, k) - zi, 0.0);
      if (hi > P.dry && hg > 0.0) { ung = un + 2.0 * (ci - sqrt(P.g * hg)); }
      if (hg > 0.0) { ung = max(ung, -sqrt(P.g * hg)); }
    }
  } else {
    qOut = bcG(9u, k);
    if (hi > P.dry) {
      if (qOut < 0.0) {
        let ci = sqrt(P.g * hi);
        if (abs(un) < ci) { hg = depthFromInvariant(un + 2.0 * ci, qOut, hi); }
        hg = max(hg, pow(qOut * qOut / P.g, 1.0 / 3.0)); // at most a critical inflow (issue #214)
      }
    } else {
      hg = pow(qOut * qOut / P.g, 1.0 / 3.0);
    }
    ung = 0.0; if (hg > 0.0) { ung = qOut / hg; }
    utg = 0.0;
    prescribed = true;
  }
  let c = bcG(10u, k);
  var Lside = Side(hi, 0.0, 0.0); var Rside = Side(hg, 0.0, 0.0);
  if (hi > P.dry && hi > 0.0) { Lside.un = un; Lside.ut = ut; }
  if (hg > P.dry && hg > 0.0) { Rside.un = ung; Rside.ut = utg; }
  var flow = 0.0;
  if (prescribed) { flow = -qOut * c * L; }
  if (hi <= 0.0 && hg <= 0.0) { return vec3<f32>(flow, 0.0, 0.0); }
  let F = hllc(Lside, Rside, hg - hi);
  if (!prescribed) { flow = -F.fh * c * L; }
  let book = -c * (F.fnL * nrm + F.ft * vec2<f32>(-nrm.y, nrm.x)) - c * 0.5 * P.g * hi * hi * nrm;
  let dq = book * (L * dt / cg(0u, i));
  return vec3<f32>(flow, dq.x, dq.y);
}
@compute @workgroup_size(WG)
fn boundary(@builtin(global_invocation_id) id: vec3<u32>) {
  let r = id.x; if (r >= P.nruns || C.doFire == 0u) { return; }
  // With the tier-0 cells: every base substep, or every tail step.
  var dt = C.dt0;
  if (C.mode == 1u) {
    if (S.x >= C.tailLeft) { return; }
    dt = C.dtTail;
  }
  let runs = P.offBcIdx + P.nbc;
  let types = runs + P.nruns + 1u;
  for (var k = idx[runs + r]; k < idx[runs + r + 1u]; k = k + 1u) {
    let i = idx[P.offBcIdx + k];
    if (cu(8u, i) == 0u || cu(11u, i) != 0u) { continue; }
    let L = bcG(0u, k);
    let n = bcG(1u, k);
    let ty = idx[types + k];
    var f = 0.0;
    setBcS(3u, k, dt);
    setBcS(2u, k, 0.0);
    let swe = (P.flags & FLAG_SWE) != 0u;
    var dq = vec2<f32>(0.0, 0.0);      // FULL_SWE: momentum booked into the cell (m^2/s)
    if (swe) {
      if (L > 1.0e-12) {
        let rb = sweBoundary(i, k, ty, L, n, dt);
        f = rb.x; dq = vec2<f32>(rb.y, rb.z);
        setCf(3u, i, cf(3u, i) + dq.x); setCf(4u, i, cf(4u, i) + dq.y);
      }
    } else if (ty == 1u) {
      // NORMAL_FLOW, signed slope: positive drains the domain, negative feeds it.
      let S = bcG(4u, k);
      let h = cf(2u, i);
      var hOut = h; // VFR_FACE: the wetted depth over the boundary edge's own beds
      if ((P.flags & FLAG_VFR_FACE) != 0u) {
        hOut = faceDepthFromEta(cf(1u, i) + bcG(5u, k), 0.0, bcG(6u, k));
      }
      if (S != 0.0 && h > 0.0 && hOut > 0.0 && n > 0.0) {
        f = -sign(S) * (hOut * pow(hOut * hOut, 1.0 / 3.0) * sqrt(abs(S)) / n) * L;
      }
    } else if (ty == 3u || ty == 4u) {
      f = -bcG(9u, k) * L; // outward discharge per metre, inflow-positive here
    } else if (ty == 2u && (P.flags & FLAG_DIFFUSIVE) != 0u) {
      // DIFFUSIVE_WAVE stage boundary: the collapsed Manning conductance against the stage,
      // with the upwind depth at the edge (DiffusiveKernels::stageBoundaryFlux).
      let etaBc = bcG(8u, k);
      let dh = cf(1u, i) - etaBc;
      let invDist = bcG(7u, k);
      if (n > 0.0 && invDist > 0.0) {
        var hUp = 0.0;
        if ((P.flags & FLAG_VFR_FACE) != 0u) {
          hUp = faceDepthFromEta(select(etaBc, cf(1u, i), dh > 0.0) + bcG(5u, k), 0.0, bcG(6u, k));
        } else if (dh > 0.0) {
          hUp = cf(2u, i);
        } else {
          hUp = max(etaBc + bcG(5u, k), 0.0);
        }
        if (hUp > 0.0) {
          var reg = sqrt(abs(dh));
          if (P.dhEps > 0.0 && abs(dh) < P.dhEps) {
            let inv = 1.0 / sqrt(P.dhEps);
            reg = (1.5 * inv) * abs(dh) - (0.5 * inv / P.dhEps) * dh * dh;
          }
          f = -hUp * pow(hUp * hUp, 1.0 / 3.0) * sign(dh) * reg * L * sqrt(invDist) / n;
        }
      }
    } else if (ty == 2u) {
      // The ghost holds the prescribed stage etaBc with a zero-gradient discharge, so the
      // time blend collapses to the edge's own q.
      let etaBc = bcG(8u, k);
      let head = cf(1u, i);
      var hf = max(head, etaBc) + bcG(5u, k);
      if ((P.flags & FLAG_VFR_FACE) != 0u) { hf = faceDepthFromEta(hf, 0.0, bcG(6u, k)); }
      if (hf <= P.dry || L <= 1.0e-12) { setBcS(0u, k, 0.0); continue; }
      var deta = head - etaBc;
      if (abs(deta) < ETA_DEADBAND) { deta = 0.0; }
      let q0 = bcS(0u, k);
      let h73 = hf * hf * pow(hf, 1.0 / 3.0);
      var qn1 = (q0 - dt * P.g * hf * deta * bcG(7u, k)) / (1.0 + P.g * dt * n * n * abs(q0) / h73);
      let qcap = P.froude * hf * sqrt(P.g * hf);
      f = clamp(qn1, -qcap, qcap) * L;
    }
    if (f == 0.0) { setBcS(0u, k, 0.0); continue; }
    let dReq = fromUnits(quantize(dt * f));
    if (order2()) { addBcU(4u, k, half64(dReq)); } else { addBcU(4u, k, dReq); }
    let v0 = vol64(i);
    var v1 = add64(v0, dReq);
    if (ty == 2u && !swe) {
      // Equilibrium clamp: one step moves the cell at most to the prescribed stage.
      let veq = fromUnits(round(cellDepthAt(i, bcG(8u, k)) * cg(0u, i) * UNITS_PER_M3));
      if (f < 0.0) { v1 = max64(v1, min64(v0, veq)); } else { v1 = min64(v1, max64(v0, veq)); }
    }
    // Availability floor: an outflow never draws on a rounding debt.
    v1 = max64(v1, min64(v0, vec2<u32>(0u, 0u)));
    let dU = add64(v1, neg64(v0));
    let applied = toM3(dU);
    let fa = applied / dt;
    if (swe && (dq.x != 0.0 || dq.y != 0.0) && !isZero64(add64(dU, neg64(dReq)))) {
      // The clamp shrank the mass: take the same fraction of the booked momentum back.
      var lam = 0.0; if (f != 0.0) { lam = fa / f; }
      setCf(3u, i, cf(3u, i) + dq.x * (lam - 1.0)); setCf(4u, i, cf(4u, i) + dq.y * (lam - 1.0));
    }
    setBcS(2u, k, fa);
    var q = 0.0; if (L > 1.0e-12) { q = fa / L; }
    setBcS(0u, k, q);
    if (P.ns > 0u) {
      // Row receipts: outflow at the cell's concentration, inflow at the edge's; then the rows
      // move with the applied water.
      let w = select(1.0, 0.5, order2());
      let req = toM3(dReq);
      for (var s = 0u; s < P.ns; s = s + 1u) {
        var c = spBcConc(k, s);
        if (req < 0.0) { c = spConc(s, i); }
        spAdd(spBc(2u, k, s), w * req * c);
        spAdd(spBc(3u, k, s), w * applied * c);
      }
      if (applied < 0.0) { spSink(i, -applied, w, 1u, k); }
      else if (applied > 0.0) { spArrive(i, applied, w, 1u, k, 0u); }
    }
    if (!isZero64(dU)) {
      setVolume(i, add64(v0, dU));
      var booked = dU;
      if (order2()) { booked = half64(dU); addCellU(18u, i, booked); }
      setBcS(1u, k, bcS(1u, k) + toM3(booked));
      if (isNeg64(booked)) { addBcU(2u, k, neg64(booked)); } else { addBcU(0u, k, booked); }
    }
    // Perot completion: the boundary edge's share the interior-only rebuild missed.
    if ((P.flags & FLAG_PEROT) != 0u && q != 0.0) { bcPerot(i, k, -fa); }
  }
}
// After a conveyance change rebuilt the Perot vectors from the interior faces: add each
// boundary edge's held discharge back (== the Kokkos refresh_boundary_discharge pass).
@compute @workgroup_size(WG)
fn refreshBoundaryPerot(@builtin(global_invocation_id) id: vec3<u32>) {
  let r = id.x; if (r >= P.nruns) { return; }
  let runs = P.offBcIdx + P.nbc;
  for (var k = idx[runs + r]; k < idx[runs + r + 1u]; k = k + 1u) {
    let i = idx[P.offBcIdx + k];
    bcPerot(i, k, -bcS(0u, k) * faceG[P.offBcG + k]);
  }
}
// ---- 1D-2D junction exchange (the Kokkos fireExchange, ordinary pass) --------
// Live exchange at the tier-0 cadence against the routing step's held 1D heads. Points that
// share a cell, a node or a read stencil form a group that fires in point order on one
// thread; groups are independent. Heads are relative to each point's datum (host header).
fn cpI(f: u32, k: u32) -> u32 { return idx[P.offCpIdx + f * P.ncp + k]; }
fn cpG(f: u32, k: u32) -> f32 { return faceG[P.offCpG + f * P.ncp + k]; }
fn cpGroupAt(g: u32) -> u32 { return idx[P.offCpIdx + 9u * P.ncp + g]; }
fn cpItem(p: u32) -> u32 { return idx[P.offCpIdx + 9u * P.ncp + P.ncpGroups + 1u + p]; }
fn stCell(p: u32) -> u32 { return idx[P.offCpIdx + 10u * P.ncp + P.ncpGroups + 1u + p]; }
fn nodeBudget(n: u32) -> vec2<u32> {
  let at = P.offCpIdx + 10u * P.ncp + P.ncpGroups + 1u + P.nst;
  return vec2<u32>(idx[at + n], idx[at + P.ncpNodes + n]);
}
fn stW(p: u32) -> f32 { return faceG[P.offCpG + 16u * P.ncp + p]; }
fn stO(p: u32) -> f32 { return faceG[P.offCpG + 16u * P.ncp + P.nst + p]; }
fn addCpU(at: u32, n: u32, v: vec2<u32>) {
  let r = add64(vec2<u32>(faceU[at], faceU[at + n]), v);
  faceU[at] = r.x; faceU[at + n] = r.y;
}
fn smooth01(t: f32) -> f32 { let u = clamp(t, 0.0, 1.0); return u * u * (3.0 - 2.0 * u); }
// exchange::phi: sqrt(x), C1-regularized below eps.
fn phiReg(x: f32, eps: f32) -> f32 {
  if (!(x > 0.0)) { return 0.0; }
  if (x >= eps) { return sqrt(x); }
  let ise = 1.0 / sqrt(eps);
  return (1.5 * ise) * x - (0.5 * ise / eps) * x * x;
}
// exchange::softmin: the smooth minimum (a^-16 + b^-16)^(-1/16) of two positive rates.
fn softmin2(a: f32, b: f32) -> f32 {
  if (!(a > 0.0) || !(b > 0.0)) { return 0.0; }
  let lo = min(a, b);
  let r = lo / max(a, b);
  var r16 = r * r; r16 = r16 * r16; r16 = r16 * r16; r16 = r16 * r16;
  return lo / sqrt(sqrt(sqrt(sqrt(1.0 + r16))));
}
// exchange::exchange(...).q, WEIR_ORIFICE: positive drains the surface into the node.
fn weirOrifice(k: u32, h2: f32, h1: f32) -> f32 {
  let s2g = sqrt(2.0 * P.g);
  let z = cpG(2u, k); let lw = cpG(3u, k); let ao = cpG(4u, k); let eps = cpG(9u, k);
  if (h2 > h1) {
    let hs = h2 - z;
    if (!(hs > 0.0)) { return 0.0; }
    // Drowned at the crest, free eps below it; the submerged forms see the crest as their
    // downstream level up to the crest and ease onto the sewer head over the eps/4 above it.
    let s = smooth01(1.0 + (h1 - z) / eps);
    let cw = 0.25 * eps;
    var x = h1 - z; var clip = 0.0;
    if (x >= cw) { clip = x; } else if (x > 0.0) { clip = x * smooth01(x / cw); }
    let dhs = hs - clip;
    let weirFree = (2.0 / 3.0) * cpG(5u, k) * lw * s2g * hs * phiReg(hs, eps);
    let weirSub = cpG(6u, k) * lw * s2g * hs * phiReg(dhs, eps);
    let orifFree = cpG(7u, k) * ao * s2g * phiReg(hs, eps);
    let orifSub = cpG(7u, k) * ao * s2g * phiReg(dhs, eps);
    return softmin2((1.0 - s) * weirFree + s * weirSub, (1.0 - s) * orifFree + s * orifSub);
  }
  if (h1 > h2 && h1 > z) {
    let hd = h1 - z;
    var s = 0.0; if (h2 > z) { s = smooth01((h2 - z) / (0.1 * hd)); }
    return -cpG(8u, k) * ao * s2g * ((1.0 - s) * phiReg(hd, eps) + s * phiReg(h1 - h2, eps));
  }
  return 0.0;
}
// The legacy ORIFICE law (computeNodeCouplingQ): the opening ramps from the inlet area to twice
// it across 0.05 m above the crown, and the flow fades in over the same band.
fn orificeLaw(k: u32, h2: f32, h1: f32) -> f32 {
  let crown = cpG(10u, k);
  let area = cpG(1u, k);
  let hmax = max(h1, h2);
  var aeff = area;
  if (hmax >= crown) { aeff = area + min((hmax - crown) / 0.05, 1.0) * area; }
  let dh = h2 - h1;
  let a = abs(dh);
  if (a < 1.0e-12) { return 0.0; }
  let q = sign(dh) * cpG(0u, k) * aeff * sqrt(2.0 * P.g) * phiReg(a, 0.02);
  return q * smooth01((hmax - crown) / 0.05);
}
// The exchange rate of point k against the current surface (computeNodeCouplingQ): the law,
// the source-side wet ramps and the COUPLING_EQUALIZE cap. Positive drains the surface.
fn exchangeRate(k: u32, ci: u32) -> f32 {
  // The surface head over the point (couplingHead2D), relative to the point's datum.
  let mode = cpI(3u, k);
  var h2 = cf(1u, ci);
  if (mode == 1u) {
    var h = cpG(11u, k);
    for (var p = cpI(4u, k); p < cpI(5u, k); p = p + 1u) { h = h + stW(p) * cf(1u, stCell(p)); }
    h2 = h;
  } else if (mode == 2u) {
    var num = 0.0; var den = 0.0;
    for (var p = cpI(4u, k); p < cpI(5u, k); p = p + 1u) {
      let t = stCell(p);
      let d = cf(2u, t);
      if (!(d >= P.dry)) { continue; }
      num = num + d * (cf(1u, t) + stO(p));
      den = den + d;
    }
    if (den > 0.0) { h2 = num / den; }
    else if ((cpI(8u, k) & 2u) == 0u) { h2 = cpG(12u, k); }
  }
  let h1 = cpG(13u, k);
  var d2 = 0.0;
  for (var p = cpI(6u, k); p < cpI(7u, k); p = p + 1u) { d2 = max(d2, cf(2u, stCell(p))); }
  var q = 0.0;
  if (cpI(2u, k) == 1u) { q = weirOrifice(k, h2, h1); } else { q = orificeLaw(k, h2, h1); }
  if (q > 0.0) { q = q * smooth01(d2 / P.dry); } else { q = q * smooth01(cpG(14u, k) / P.dry); }
  let eqf = cpG(15u, k);
  if ((cpI(8u, k) & 1u) != 0u && eqf > 0.0) {
    let cap = abs(h2 - h1) * eqf;
    q = clamp(q, -cap, cap);
  }
  return q;
}
// faceU tail of the exchange: exchanged per point (2 ncp) | drawn per node (2 nn) | drawn at
// half weight over an RK2 step (2 nn) | the rate kept at an RK2 stage's start (ncp, f32 bits).
fn drawnHalfAt() -> u32 { return P.offCpU + 2u * P.ncp + 2u * P.ncpNodes; }
fn keptAt() -> u32 { return P.offCpU + 2u * P.ncp + 4u * P.ncpNodes; }
// One pass of the junction exchange (fireExchange). S.y selects it under RECONSTRUCTION_ORDER 2:
// 3 evaluates every point's rate at the stage's start and keeps it; 4 applies the kept rates,
// booking half of what moves (the step is the average of its two stages). Otherwise the rate is
// evaluated and applied at once.
@compute @workgroup_size(WG)
fn exchange(@builtin(global_invocation_id) id: vec3<u32>) {
  let g = id.x; if (g >= P.ncpGroups || C.doFire == 0u) { return; }
  var dt = C.dt0;
  if (C.mode == 1u) {
    if (S.x >= C.tailLeft) { return; }
    dt = C.dtTail;
  }
  let exchAt = P.offCpU;
  let drawnAt = P.offCpU + 2u * P.ncp;
  for (var gi = cpGroupAt(g); gi < cpGroupAt(g + 1u); gi = gi + 1u) {
    let k = cpItem(gi);
    let ci = cpI(0u, k);
    if (ci == 0xffffffffu) { continue; }
    if (S.y == 3u) { faceU[keptAt() + k] = bitcast<u32>(exchangeRate(k, ci)); continue; }
    if (cu(8u, ci) == 0u) { continue; }
    var q = 0.0;
    if (S.y == 4u) { q = bitcast<f32>(faceU[keptAt() + k]); } else { q = exchangeRate(k, ci); }
    if (q == 0.0) { continue; }
    let v0 = vol64(ci);
    let zero = vec2<u32>(0u, 0u);
    var dU = zero; // into the cell
    if (q > 0.0) {
      // 2D -> 1D drain: at most the exchange-beta share of the cell's water.
      q = min(q, P.beta * max(toM3(v0), 0.0) / dt);
      dU = neg64(min64(fromUnits(quantize(q * dt)), max64(v0, zero)));
    } else {
      // 1D -> 2D spill: at most the node's ponded water not yet spilled this advance.
      let n = cpI(1u, k);
      let avail = add64(nodeBudget(n), neg64(vec2<u32>(faceU[drawnAt + n], faceU[drawnAt + P.ncpNodes + n])));
      if (!lt64(zero, avail)) { continue; }
      dU = min64(fromUnits(quantize(-q * dt)), avail);
      addCpU(drawnAt + n, P.ncpNodes, dU);
      if (S.y == 4u) { addCpU(drawnHalfAt() + n, P.ncpNodes, half64(dU)); }
    }
    if (isZero64(dU)) { continue; }
    if (P.ns > 0u) {
      // A drain leaves at the cell's concentration, booked per point for the node; a spill
      // arrives at the node's row values, frozen for the advance.
      let w = select(1.0, 0.5, S.y == 4u);
      let dv = toM3(dU);
      if (q > 0.0) {
        spSink(ci, -dv, w, 0u, k);
      } else if ((P.spFlags & SP_NODE_CONC) != 0u) {
        spArrive(ci, dv, w, 0u, k, cpI(1u, k));
        spAdd(spCpSpill(k), w * dv);
      }
    }
    setVolume(ci, add64(v0, dU));
    var booked = dU;
    if (S.y == 4u) { booked = half64(dU); addCellU(18u, ci, booked); }
    addCpU(exchAt + k, P.ncp, neg64(booked)); // positive: 2D -> 1D
    addCellU(6u, ci, booked);                 // the coupling ledger, positive into the cell
  }
}
// ---- RECONSTRUCTION_ORDER 2: SSP-RK2 steps (runRk2Step) and limited gradients -------
// The start of a step: keep the volume and momentum, clear the step's half accumulator.
@compute @workgroup_size(WG)
fn rkSnapshot(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (C.doFire == 0u) { return; }
  if (i < P.ncpNodes) {
    // The node draw at half weight starts from the draw so far.
    let d = P.offCpU + 2u * P.ncp;
    faceU[drawnHalfAt() + i] = faceU[d + i];
    faceU[drawnHalfAt() + P.ncpNodes + i] = faceU[d + P.ncpNodes + i];
  }
  if (i >= P.nc) { return; }
  setCu(16u, i, cu(0u, i)); setCu(17u, i, cu(1u, i));
  setCu(18u, i, 0u); setCu(19u, i, 0u);
  setCf(14u, i, cf(3u, i)); setCf(15u, i, cf(4u, i));
  for (var s = 0u; s < P.ns; s = s + 1u) { faceS[spMass0(s, i)] = spMass(s, i); }
}
// The end of a step: an active cell takes the beginning volume plus the halves of every stage
// booking (the average of the beginning and second-stage states, exactly), and the mean of the
// beginning and second-stage momentum; a dry cell carries no momentum.
@compute @workgroup_size(WG)
fn rkAverage(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (C.doFire == 0u) { return; }
  if (i < P.ncpNodes) {
    // The node draw of the step: the stages' halves.
    let d = P.offCpU + 2u * P.ncp;
    faceU[d + i] = faceU[drawnHalfAt() + i];
    faceU[d + P.ncpNodes + i] = faceU[drawnHalfAt() + P.ncpNodes + i];
  }
  if (i >= P.nc) { return; }
  if (cu(8u, i) != 0u) {
    setVolume(i, add64(vec2<u32>(cu(16u, i), cu(17u, i)), vec2<u32>(cu(18u, i), cu(19u, i))));
    setCf(3u, i, 0.5 * (cf(14u, i) + cf(3u, i)));
    setCf(4u, i, 0.5 * (cf(15u, i) + cf(4u, i)));
  }
  if (cf(2u, i) <= P.dry) { setCf(3u, i, 0.0); setCf(4u, i, 0.0); }
  // Every row inventory is the mean of the step's start and its second stage (the totals were
  // booked at half weight).
  for (var s = 0u; s < P.ns; s = s + 1u) { setSpMass(s, i, 0.5 * (faceS[spMass0(s, i)] + spMass(s, i))); }
}
fn vb(v: u32, m: u32) -> f32 { return cellF[P.offVb + 4u * v + m]; }
// Velocity bounds at each vertex over the wet active cells that reach it (above its bed).
@compute @workgroup_size(WG)
fn vertexBounds(@builtin(global_invocation_id) id: vec3<u32>) {
  let v = id.x; if (v >= P.nv || C.doFire == 0u) { return; }
  var ulo = 1.0e30; var uhi = -1.0e30; var vlo = 1.0e30; var vhi = -1.0e30;
  let base = P.offVsPtr + P.nv + 1u;
  for (var p = idx[P.offVsPtr + v]; p < idx[P.offVsPtr + v + 1u]; p = p + 1u) {
    let j = idx[base + p];
    let h = cf(2u, j);
    if (cu(8u, j) == 0u || h <= P.dry || cf(1u, j) + faceG[P.offVsOff + p] <= P.dry) { continue; }
    let u = cf(3u, j) / h; let w = cf(4u, j) / h;
    ulo = min(ulo, u); uhi = max(uhi, u); vlo = min(vlo, w); vhi = max(vhi, w);
  }
  cellF[P.offVb + 4u * v] = ulo; cellF[P.offVb + 4u * v + 1u] = uhi;
  cellF[P.offVb + 4u * v + 2u] = vlo; cellF[P.offVb + 4u * v + 3u] = vhi;
}
// swe::bjLimiter (Barth-Jespersen).
fn bj(w: f32, wf: f32, wmin: f32, wmax: f32) -> f32 {
  let d = wf - w;
  if (d > 1.0e-14) { return min(1.0, (wmax - w) / d); }
  if (d < -1.0e-14) { return min(1.0, (wmin - w) / d); }
  return 1.0;
}
fn cellArm(i: u32, k: u32, c: u32) -> f32 { return faceG[P.offCellArm + (3u * k + c) * P.nc + i]; }
// Limited gradients of (eta, u, v, h) (computeLimitedGradientsSwe): weighted least squares over
// the connected wet neighbors, Barth-Jespersen limited at the face midpoints, velocities also at
// the vertices against the vertex bounds, depth kept positive at the edge midpoints. eta is
// taken relative to the cell's own head (neighbors through the face's reference-bed step), so
// the differences are formed from small numbers.
@compute @workgroup_size(WG)
fn gradients(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc || C.doFire == 0u || cu(8u, i) == 0u) { return; }
  for (var f = 6u; f < 14u; f = f + 1u) { setCf(f, i, 0.0); }
  let hi = cf(2u, i);
  if (hi <= 10.0 * P.dry) { return; }
  var wi = array<f32, 4>(0.0, cf(3u, i) / hi, cf(4u, i) / hi, hi);
  var lo = wi; var hw = wi;
  var xx = 0.0; var xy = 0.0; var yy = 0.0;
  var bx = array<f32, 4>(0.0, 0.0, 0.0, 0.0); var by = array<f32, 4>(0.0, 0.0, 0.0, 0.0);
  var connected = 0u; var count = 0u; var shore = false;
  let begin = rowBegin(i);
  for (var p = begin; p < rowEnd(i); p = p + 1u) {
    let e = rowCode(p) & 0x7fffffffu;
    var j = cL(e); var dref = -fg(10u, e);
    if (j == i) { j = cR(e); dref = fg(10u, e); }
    let hj = cf(2u, j);
    let ej = (cf(1u, j) - cf(1u, i)) + dref;
    let sill = max(-hi, ej - hj);
    if (hj <= P.dry || cu(8u, j) == 0u || -sill <= P.dry || ej - sill <= P.dry) { shore = true; continue; }
    connected = connected | (1u << (p - begin));
    count = count + 1u;
    let dx = cg(2u, j) - cg(2u, i); let dy = cg(3u, j) - cg(3u, i);
    let wt = 1.0 / (dx * dx + dy * dy);
    xx = xx + wt * dx * dx; xy = xy + wt * dx * dy; yy = yy + wt * dy * dy;
    let wj = array<f32, 4>(ej, cf(3u, j) / hj, cf(4u, j) / hj, hj);
    for (var m = 0u; m < 4u; m = m + 1u) {
      bx[m] = bx[m] + wt * dx * (wj[m] - wi[m]);
      by[m] = by[m] + wt * dy * (wj[m] - wi[m]);
      lo[m] = min(lo[m], wj[m]); hw[m] = max(hw[m], wj[m]);
    }
  }
  if (count < 2u) { return; }
  let det = xx * yy - xy * xy;
  if (det <= 1.0e-2 * (xx + yy) * (xx + yy)) { return; } // shoreline conditioning bound
  var gx = array<f32, 4>(0.0, 0.0, 0.0, 0.0); var gy = array<f32, 4>(0.0, 0.0, 0.0, 0.0);
  var phi = array<f32, 4>(1.0, 1.0, 1.0, 1.0);
  for (var m = 0u; m < 4u; m = m + 1u) {
    if (shore && (m == 1u || m == 2u)) { continue; }
    gx[m] = (yy * bx[m] - xy * by[m]) / det;
    gy[m] = (xx * by[m] - xy * bx[m]) / det;
  }
  for (var p = begin; p < rowEnd(i); p = p + 1u) {
    if ((connected & (1u << (p - begin))) == 0u) { continue; }
    let ax = faceG[P.offCsrArm + p]; let ay = faceG[P.offCsrArm + P.ncsr + p];
    for (var m = 0u; m < 4u; m = m + 1u) {
      phi[m] = min(phi[m], bj(wi[m], wi[m] + gx[m] * ax + gy[m] * ay, lo[m], hw[m]));
    }
  }
  let nvc = select(3u, 4u, isQuad(i));
  if (!shore) {
    for (var k = 0u; k < nvc; k = k + 1u) {
      let v = idx[P.offCellV + 4u * i + k];
      if (cf(1u, i) + cellArm(i, k, 2u) <= P.dry) { phi[1] = 0.0; phi[2] = 0.0; break; }
      let ax = cellArm(i, k, 0u); let ay = cellArm(i, k, 1u);
      phi[1] = min(phi[1], bj(wi[1], wi[1] + gx[1] * ax + gy[1] * ay, min(vb(v, 0u), wi[1]), max(wi[1], vb(v, 1u))));
      phi[2] = min(phi[2], bj(wi[2], wi[2] + gx[2] * ax + gy[2] * ay, min(vb(v, 2u), wi[2]), max(wi[2], vb(v, 3u))));
    }
  }
  if (count < nvc) {
    var positive = 1.0;
    for (var k = 0u; k < nvc; k = k + 1u) {
      let k2 = (k + 1u) % nvc;
      let ax = 0.5 * (cellArm(i, k, 0u) + cellArm(i, k2, 0u));
      let ay = 0.5 * (cellArm(i, k, 1u) + cellArm(i, k2, 1u));
      let dh = phi[3] * (gx[3] * ax + gy[3] * ay);
      if (dh < -hi) { positive = min(positive, -hi / dh); }
    }
    phi[0] = phi[0] * positive; phi[3] = phi[3] * positive;
  }
  setCf(6u, i, phi[0] * gx[0]); setCf(7u, i, phi[0] * gy[0]);
  setCf(8u, i, phi[3] * gx[3]); setCf(9u, i, phi[3] * gy[3]);
  setCf(10u, i, phi[1] * gx[1]); setCf(11u, i, phi[1] * gy[1]);
  setCf(12u, i, phi[2] * gx[2]); setCf(13u, i, phi[2] * gy[2]);
}
// ---- momentum physics (Coriolis, eddy viscosity, wall drag; FULL_SWE with one tier) ----
// The host drives these like the FP64 marchers' runMomentumStep / applyMomentumPhysics /
// physicsAdvance, reading one reduction back per decision. Parameters ride in Ctl.ph*.
fn phf(f: u32, i: u32) -> f32 { return cellF[P.offPhF + f * P.nc + i]; }
fn setPhf(f: u32, i: u32, v: f32) { cellF[P.offPhF + f * P.nc + i] = v; }
fn stGxAt(i: u32, k: u32) -> u32 { return P.offPhF + 14u * P.nc + 5u * i + k; }
fn stGyAt(i: u32, k: u32) -> u32 { return P.offPhF + 19u * P.nc + 5u * i + k; }
fn stressAt(f: u32, e: u32) -> u32 { return P.offPhF + 24u * P.nc + f * P.ne + e; }
fn stIdAt(i: u32, k: u32) -> u32 { return P.offPhU + 5u * i + k; }
fn stNAt(i: u32) -> u32 { return P.offPhU + 5u * P.nc + i; }
fn connAt(e: u32) -> u32 { return P.offPhU + 6u * P.nc + e; }
// The velocity source of an evaluation: S.y 0 the cell momentum, 1 the predictor's.
fn phQ(i: u32) -> vec2<f32> {
  if (S.y == 1u) { return vec2<f32>(phf(6u, i), phf(7u, i)); }
  return vec2<f32>(cf(3u, i), cf(4u, i));
}
@compute @workgroup_size(1)
fn phReset() {
  atomicStore(&C.phStableBits, 0x7f800000u);
  atomicStore(&C.phRateBits, 0u);
  atomicStore(&C.phBad, 0u);
}
@compute @workgroup_size(WG)
fn phSave(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc) { return; }
  setPhf(0u, i, cf(3u, i)); setPhf(1u, i, cf(4u, i));
}
@compute @workgroup_size(WG)
fn phRestore(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc) { return; }
  setCf(3u, i, phf(0u, i)); setCf(4u, i, phf(1u, i));
}
// Coriolis: rotate every cell's momentum by the angle whose cosine and sine the host set.
@compute @workgroup_size(WG)
fn phRotate(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc) { return; }
  let x = cf(3u, i); let y = cf(4u, i);
  setCf(3u, i, C.phCos * x + C.phSin * y);
  setCf(4u, i, C.phCos * y - C.phSin * x);
}
// Quadratic wall traction over C.phSeconds on the WALL_DRAG edges: each cell sweeps its edges
// forward, then in reverse (physicsWallDrag).
@compute @workgroup_size(WG)
fn phWallDrag(@builtin(global_invocation_id) id: vec3<u32>) {
  let g = id.x; if (g >= P.nwdGroups) { return; }
  let first = idx[P.offWdIdx + P.nwd + g]; let last = idx[P.offWdIdx + P.nwd + g + 1u];
  for (var sweep = 0u; sweep < 2u; sweep = sweep + 1u) {
    for (var step = 0u; step < last - first; step = step + 1u) {
      var s = first + step; if (sweep == 1u) { s = last - 1u - step; }
      let i = idx[P.offWdIdx + s];
      let h = cf(2u, i);
      if (h <= P.dry) { continue; }
      var hw = h;
      if ((P.flags & FLAG_VFR) != 0u) {
        hw = faceDepthFromEta(cf(1u, i) + faceG[P.offWdG + 4u * P.nwd + s], 0.0, faceG[P.offWdG + 5u * P.nwd + s]);
      }
      let k = faceG[P.offWdG + 3u * P.nwd + s] * faceG[P.offWdG + 2u * P.nwd + s] * hw / (cg(0u, i) * h);
      if (k == 0.0) { continue; }
      let t = vec2<f32>(-faceG[P.offWdG + P.nwd + s], faceG[P.offWdG + s]);
      let ut = (cf(3u, i) * t.x + cf(4u, i) * t.y) / h;
      if (ut == 0.0) { continue; }
      let next = sign(ut) * (1.0 / (1.0 / abs(ut) + 0.5 * C.phSeconds * k));
      let dq = h * (next - ut);
      setCf(3u, i, cf(3u, i) + dq * t.x); setCf(4u, i, cf(4u, i) + dq * t.y);
    }
  }
}
// A face takes part in mixing when both cells are wet and the water stands above the higher bed.
@compute @workgroup_size(WG)
fn phConnected(@builtin(global_invocation_id) id: vec3<u32>) {
  let e = id.x; if (e >= P.ne) { return; }
  let a = cL(e); let b = cR(e);
  let ha = cf(2u, a); let hb = cf(2u, b);
  let ea = cf(1u, a) + fg(1u, e); let eb = cf(1u, b) + fg(9u, e); // the face frame
  let sill = max(ea - ha, eb - hb);
  cellU[connAt(e)] = select(0u, 1u, fg(8u, e) > 0.0 && ha > P.dry && hb > P.dry && min(ea, eb) - sill > P.dry);
}
fn eddyViscosity(i: u32, ux: f32, uy: f32, vx: f32, vy: f32) -> f32 {
  let strain = sqrt(2.0 * ux * ux + 2.0 * vy * vy + (uy + vx) * (uy + vx));
  let vorticity = abs(vx - uy);
  if (C.phDc > 0.0) {
    let l = min(0.41 * faceG[P.offWalld + i], C.phDc * cf(2u, i));
    return C.phBg + l * l * vorticity;
  }
  return C.phBg + C.phCs * C.phCs * cg(0u, i) * strain;
}
// Least-squares velocity stencils over the connected neighbors and the eddy viscosity.
@compute @workgroup_size(WG)
fn phStencil(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc) { return; }
  setPhf(8u, i, 0.0);
  for (var k = 0u; k < 5u; k = k + 1u) { cellU[stIdAt(i, k)] = 0u; cellF[stGxAt(i, k)] = 0.0; cellF[stGyAt(i, k)] = 0.0; }
  cellU[stNAt(i)] = 0u;
  cellU[ownAt(i)] = 0u;
  if (cf(2u, i) <= P.dry) { return; }
  var ids = array<u32, 5>(i, 0u, 0u, 0u, 0u);
  var gx = array<f32, 5>(0.0, 0.0, 0.0, 0.0, 0.0); var gy = array<f32, 5>(0.0, 0.0, 0.0, 0.0, 0.0);
  var count = 1u; var xx = 0.0; var xy = 0.0; var yy = 0.0;
  for (var p = rowBegin(i); p < rowEnd(i); p = p + 1u) {
    let e = rowCode(p) & 0x7fffffffu;
    if (cellU[connAt(e)] == 0u) { continue; }
    let j = neighbor(i, p);
    let dx = cg(2u, j) - cg(2u, i); let dy = cg(3u, j) - cg(3u, i);
    let w = 1.0 / (dx * dx + dy * dy);
    ids[count] = j; gx[count] = w * dx; gy[count] = w * dy; count = count + 1u;
    xx = xx + w * dx * dx; xy = xy + w * dx * dy; yy = yy + w * dy * dy;
  }
  let det = xx * yy - xy * xy;
  let single = count < 3u || det <= 1.0e-10 * (xx + yy) * (xx + yy);
  if (!single) {
    for (var k = 1u; k < count; k = k + 1u) {
      let x = gx[k]; let y = gy[k];
      gx[k] = (yy * x - xy * y) / det; gy[k] = (xx * y - xy * x) / det;
      gx[0] = gx[0] - gx[k]; gy[0] = gy[0] - gy[k];
    }
  }
  for (var k = 0u; k < 5u; k = k + 1u) { cellU[stIdAt(i, k)] = ids[k]; cellF[stGxAt(i, k)] = gx[k]; cellF[stGyAt(i, k)] = gy[k]; }
  cellU[ownAt(i)] = select(1u, 2u, single);
  if (single) { cellU[stNAt(i)] = 1u; return; }
  cellU[stNAt(i)] = count;
  var ux = 0.0; var uy = 0.0; var vx = 0.0; var vy = 0.0;
  for (var k = 0u; k < count; k = k + 1u) {
    let j = ids[k]; let q = phQ(j) / cf(2u, j);
    ux = ux + gx[k] * q.x; uy = uy + gy[k] * q.x; vx = vx + gx[k] * q.y; vy = vy + gy[k] * q.y;
  }
  let nu = eddyViscosity(i, ux, uy, vx, vy);
  if (!(abs(nu) <= 3.0e38)) { atomicStore(&C.phBad, 1u); }
  setPhf(8u, i, nu);
}
// Per block of 256 cells: whether it holds a corner still unsettled (phSingle visits only those
// blocks). Per cell (ownAt): 0 dry; 1 built its own complete stencil; 2 a corner (wet, one-cell
// stencil) not settled yet; 4 + 2r a corner that borrowed in round r; 5 + 2r one that found
// nothing to borrow in round r.
fn blockFlagAt(k: u32) -> u32 { return P.offPhU + 6u * P.nc + P.ne + k; }
fn ownAt(i: u32) -> u32 { return P.offPhU + 6u * P.nc + P.ne + (P.nc + 255u) / 256u + i; }
fn viscosityFromStencil(i: u32) -> f32 {
  var ux = 0.0; var uy = 0.0; var vx = 0.0; var vy = 0.0;
  for (var k = 0u; k < cellU[stNAt(i)]; k = k + 1u) {
    let j = cellU[stIdAt(i, k)]; let q = phQ(j) / cf(2u, j);
    ux = ux + cellF[stGxAt(i, k)] * q.x; uy = uy + cellF[stGyAt(i, k)] * q.x;
    vx = vx + cellF[stGxAt(i, k)] * q.y; vy = vy + cellF[stGyAt(i, k)] * q.y;
  }
  return eddyViscosity(i, ux, uy, vx, vy);
}
fn borrowStencil(i: u32, j: u32) {
  for (var k = 0u; k < 5u; k = k + 1u) {
    cellU[stIdAt(i, k)] = cellU[stIdAt(j, k)];
    cellF[stGxAt(i, k)] = cellF[stGxAt(j, k)]; cellF[stGyAt(i, k)] = cellF[stGyAt(j, k)];
  }
  cellU[stNAt(i)] = cellU[stNAt(j)];
}
// The FP64 marchers' corner rule: in ascending cell order, a corner borrows the complete stencil
// of its first connected neighbor (face order) that has one at that moment, i.e. a cell that
// built its own or a lower-numbered corner that borrowed. A corner's choice depends only on
// lower-numbered corners, so it is settled exactly in round r once those ahead of its choice
// settled in an earlier round (a corner settling in round r is read as unsettled in round r).
// Returns 0 not yet, 1 borrowed, 2 nothing to borrow (the one-cell stencil stays).
fn cornerChoice(i: u32, round: u32) -> u32 {
  for (var p = rowBegin(i); p < rowEnd(i); p = p + 1u) {
    let e = rowCode(p) & 0x7fffffffu;
    if (cellU[connAt(e)] == 0u) { continue; }
    let j = neighbor(i, p);
    let o = cellU[ownAt(j)];
    if (o == 1u) { borrowStencil(i, j); return 1u; }
    if (o < 2u || j > i) { continue; } // the serial sweep has not reached a later corner
    if (o < 4u || ((o - 4u) >> 1u) >= round) { return 0u; }
    if ((o & 1u) == 0u) { borrowStencil(i, j); return 1u; }
  }
  return 2u;
}
// Round S.x of the corner rule, in parallel; kChainRounds rounds, then phSingle settles the rest.
@compute @workgroup_size(WG)
fn phChain(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc || cellU[ownAt(i)] != 2u) { return; }
  let c = cornerChoice(i, S.x);
  if (c == 0u) { return; }
  setPhf(8u, i, viscosityFromStencil(i));
  cellU[ownAt(i)] = 4u + 2u * S.x + (c - 1u);
}
@compute @workgroup_size(WG)
fn phSingleFlags(@builtin(global_invocation_id) id: vec3<u32>) {
  let k = id.x; if (k >= (P.nc + 255u) / 256u) { return; }
  var any = 0u;
  for (var i = 256u * k; i < min(256u * (k + 1u), P.nc) && any == 0u; i = i + 1u) {
    any = select(0u, 1u, cellU[ownAt(i)] == 2u);
  }
  cellU[blockFlagAt(k)] = any;
}
// The corners still unsettled after the rounds, in ascending order on one thread (the serial
// sweep itself): S.x = kChainRounds + 1, so every corner settled before is final.
@compute @workgroup_size(1)
fn phSingle() {
  for (var i = 0u; i < P.nc; i = i + 1u) {
    if ((i & 255u) == 0u && cellU[blockFlagAt(i >> 8u)] == 0u) { i = i + 255u; continue; }
    if (cellU[ownAt(i)] != 2u) { continue; }
    var c = cornerChoice(i, S.x);
    if (c == 0u) { c = 2u; } // cannot happen: every lower-numbered corner is final here
    setPhf(8u, i, viscosityFromStencil(i));
    cellU[ownAt(i)] = 4u + 2u * (S.x - 1u) + (c - 1u);
  }
}
// The viscous stress through a connected face from the merged two-cell stencil, with the
// normal-derivative correction, and its contribution to the step bound.
@compute @workgroup_size(WG)
fn phFaceStress(@builtin(global_invocation_id) id: vec3<u32>) {
  let e = id.x; if (e >= P.ne) { return; }
  for (var f = 0u; f < 4u; f = f + 1u) { cellF[stressAt(f, e)] = 0.0; }
  if (cellU[connAt(e)] == 0u) { return; }
  let i = cL(e); let j = cR(e);
  let nx = fg(4u, e); let ny = fg(5u, e);
  let dx = cg(2u, j) - cg(2u, i); let dy = cg(3u, j) - cg(3u, i);
  let dn = dx * nx + dy * ny;
  if (!(dn > 0.0)) { atomicStore(&C.phBad, 1u); return; }
  var ids = array<u32, 12>(0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
  var gx = array<f32, 12>(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
  var gy = array<f32, 12>(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
  var count = 0u;
  let fx = fg(6u, e); let fy = fg(7u, e);
  let di = length(vec2<f32>(fx - cg(2u, i), fy - cg(3u, i)));
  let dj = length(vec2<f32>(fx - cg(2u, j), fy - cg(3u, j)));
  let wi = dj / (di + dj); let wj = 1.0 - wi;
  for (var side = 0u; side < 2u; side = side + 1u) {
    var cell = i; var w = wi; if (side == 1u) { cell = j; w = wj; }
    for (var k = 0u; k < cellU[stNAt(cell)]; k = k + 1u) {
      let sid = cellU[stIdAt(cell, k)];
      var m = 0u;
      while (m < count && ids[m] != sid) { m = m + 1u; }
      if (m == count) { ids[m] = sid; count = count + 1u; }
      gx[m] = gx[m] + w * cellF[stGxAt(cell, k)]; gy[m] = gy[m] + w * cellF[stGyAt(cell, k)];
    }
  }
  for (var side = 0u; side < 2u; side = side + 1u) {
    var sid = i; if (side == 1u) { sid = j; }
    var m = 0u;
    while (m < count && ids[m] != sid) { m = m + 1u; }
    if (m == count) { ids[m] = sid; count = count + 1u; }
  }
  let nu = wi * phf(8u, i) + wj * phf(8u, j);
  let ea = cf(1u, i) + fg(1u, e); let eb = cf(1u, j) + fg(9u, e);
  let wet = max(0.0, min(ea, eb) - max(ea - cf(2u, i), eb - cf(2u, j)));
  let weight = fg(0u, e) * wet * nu * fg(8u, e);
  var ux = 0.0; var uy = 0.0; var vx = 0.0; var vy = 0.0; var bx = 0.0; var by = 0.0;
  for (var k = 0u; k < count; k = k + 1u) {
    let sid = ids[k];
    let delta = select(0.0, 1.0, sid == j) - select(0.0, 1.0, sid == i);
    let corr = (delta - gx[k] * dx - gy[k] * dy) / dn;
    let gxk = gx[k] + nx * corr; let gyk = gy[k] + ny * corr;
    let q = phQ(sid) / cf(2u, sid);
    ux = ux + gxk * q.x; uy = uy + gyk * q.x; vx = vx + gxk * q.y; vy = vy + gyk * q.y;
    bx = bx + abs(2.0 * nx * gxk + ny * gyk) + abs(ny * gxk);
    by = by + abs(nx * gyk) + abs(nx * gxk + 2.0 * ny * gyk);
  }
  cellF[stressAt(0u, e)] = weight * (2.0 * ux * nx + (uy + vx) * ny);
  cellF[stressAt(1u, e)] = weight * (2.0 * vy * ny + (uy + vx) * nx);
  cellF[stressAt(2u, e)] = weight * bx;
  cellF[stressAt(3u, e)] = weight * by;
}
// Each cell gathers its faces' stress into its rate (S.y 0: r, 1: s) and the step bound's rate.
@compute @workgroup_size(WG)
fn phGather(@builtin(global_invocation_id) id: vec3<u32>) {
  let c = id.x; if (c >= P.nc) { return; }
  var sx = 0.0; var sy = 0.0; var bx = 0.0; var by = 0.0;
  let a = cg(0u, c); let mass = a * cf(2u, c);
  for (var p = rowBegin(c); p < rowEnd(c); p = p + 1u) {
    let code = rowCode(p);
    let e = code & 0x7fffffffu;
    if (cellU[connAt(e)] == 0u) { continue; }
    var sgn = 1.0; if ((code & 0x80000000u) != 0u) { sgn = -1.0; }
    sx = sx + sgn * cellF[stressAt(0u, e)] / a;
    sy = sy + sgn * cellF[stressAt(1u, e)] / a;
    bx = bx + cellF[stressAt(2u, e)] / mass;
    by = by + cellF[stressAt(3u, e)] / mass;
  }
  if (S.y == 1u) { setPhf(4u, c, sx); setPhf(5u, c, sy); } else { setPhf(2u, c, sx); setPhf(3u, c, sy); }
  let b = max(max(bx, by), 0.0);
  if (!(b <= 3.0e38)) { atomicStore(&C.phBad, 1u); return; }
  atomicMax(&C.phRateBits, bitcast<u32>(b));
}
// Heun's predictor, corrector and acceptance over C.phDt.
@compute @workgroup_size(WG)
fn phPredict(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc) { return; }
  setPhf(6u, i, cf(3u, i) + C.phDt * phf(2u, i)); setPhf(7u, i, cf(4u, i) + C.phDt * phf(3u, i));
}
@compute @workgroup_size(WG)
fn phCorrect(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc) { return; }
  setPhf(6u, i, 0.5 * (cf(3u, i) + phf(6u, i) + C.phDt * phf(4u, i)));
  setPhf(7u, i, 0.5 * (cf(4u, i) + phf(7u, i) + C.phDt * phf(5u, i)));
}
@compute @workgroup_size(WG)
fn phAccept(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc) { return; }
  setCf(3u, i, phf(6u, i)); setCf(4u, i, phf(7u, i));
}
// The kinetic energy of the velocity source (S.y), summed deterministically: a tree per
// workgroup, then the partials in order.
var<workgroup> phShared: array<f32, 256>;
@compute @workgroup_size(WG)
fn phEnergyPart(@builtin(global_invocation_id) id: vec3<u32>, @builtin(local_invocation_id) lid: vec3<u32>,
                @builtin(workgroup_id) wid: vec3<u32>) {
  let i = id.x;
  var term = 0.0;
  if (i < P.nc && cf(2u, i) > P.dry) { let q = phQ(i); term = 0.5 * cg(0u, i) * dot(q, q) / cf(2u, i); }
  phShared[lid.x] = term;
  workgroupBarrier();
  for (var stride = 128u; stride > 0u; stride = stride >> 1u) {
    if (lid.x < stride) { phShared[lid.x] = phShared[lid.x] + phShared[lid.x + stride]; }
    workgroupBarrier();
  }
  if (lid.x == 0u) { cellF[P.offPhF + 24u * P.nc + 4u * P.ne + wid.x] = phShared[0]; }
}
@compute @workgroup_size(1)
fn phEnergySum() {
  var e = 0.0;
  let n = (P.nc + 255u) / 256u;
  for (var k = 0u; k < n; k = k + 1u) { e = e + cellF[P.offPhF + 24u * P.nc + 4u * P.ne + k]; }
  C.phEnergy = e;
}
// The wave CFL after a trial's first half: the minimum over wet cells.
@compute @workgroup_size(WG)
fn phWaveCfl(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc) { return; }
  let h = cf(2u, i);
  if (h <= P.dry) { return; }
  let dt = cellCflDt(cg(4u, i), h, length(vec2<f32>(cf(3u, i), cf(4u, i))) / h);
  atomicMin(&C.phStableBits, bitcast<u32>(dt));
}
@compute @workgroup_size(1)
fn advanceClock() {
  if (C.doFire == 0u) { planCycle(); return; }
  if (C.mode == 0u) {
    clockAdd(C.dt0 * f32(P.nsub));
    C.steps = C.steps + P.nsub * select(1u, 2u, order2()); // an RK2 step counts two
    for (var k = 0u; k < P.tiers; k = k + 1u) {
      C.faceSteps = C.faceSteps + C.facesTierLast[k] * (P.nsub >> k);
    }
    C.lastDt = C.dt0;
    C.cycles = C.cycles + 1u;
  } else if (C.mode == 1u) {
    let fired = min(C.tailLeft, P.nsub);
    C.steps = C.steps + fired * select(1u, 2u, order2());
    C.faceSteps = C.faceSteps + fired * C.nFacesLast;
    C.lastDt = C.dtTail;
    C.tailLeft = C.tailLeft - fired;
    if (C.tailLeft == 0u) {
      C.cycles = C.cycles + 1u; // preserve the rebuild cadence
      land();                   // exact landing
    } else {
      clockAdd(C.dtTail * f32(fired));
    }
  }
  planCycle();
}

// ---- initialization, conveyance changes, publication ------------------------
@compute @workgroup_size(WG)
fn reconstructAll(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc) { return; }
  setVolume(i, vol64(i));
}
@compute @workgroup_size(WG)
fn closeFaces(@builtin(global_invocation_id) id: vec3<u32>) {
  let e = id.x; if (e >= P.ne) { return; }
  if (fg(8u, e) == 0.0) { faceS[e] = 0.0; }
}
@compute @workgroup_size(WG)
fn refreshPerot(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x; if (i >= P.nc) { return; }
  perotRefresh(i);
}
// The face flux F (m3/s, positive cL to cR) the host scatters to the two edge slots as -F and
// +F. Like the other marchers, only faces with both cells active publish; the rest are zero.
@compute @workgroup_size(WG)
fn publishFlux(@builtin(global_invocation_id) id: vec3<u32>) {
  let e = id.x; if (e >= P.ne || C.finishing == 0u) { return; }
  let a = cL(e); let b = cR(e);
  if (cu(8u, a) == 0u || cu(8u, b) == 0u) { faceS[P.ne + e] = 0.0; return; }
  if ((P.flags & (FLAG_DIFFUSIVE | FLAG_SWE)) != 0u) {
    // Their face discharge is already limited and scaled by the conveyance.
    faceS[P.ne + e] = faceS[e] * fg(0u, e);
    return;
  }
  let hf = faceFlowDepth(e, cf(1u, a), cf(1u, b));
  var qp = 0.0;
  if (hf > P.dry) {
    let qcap = P.froude * hf * sqrt(P.g * hf);
    qp = clamp(faceS[e], -qcap, qcap);
  }
  faceS[P.ne + e] = qp * fg(0u, e) * fg(8u, e);
}
// Transport rows, every round: fold each per-cell ledger row over 256 cells (a fixed tree, so a
// total does not depend on the dispatch) into one partial the host sums in group order, and
// clear the cells' slots.
@compute @workgroup_size(WG)
fn spFold(@builtin(global_invocation_id) id: vec3<u32>, @builtin(local_invocation_id) lid: vec3<u32>,
          @builtin(workgroup_id) wid: vec3<u32>) {
  let i = id.x;
  for (var r = 0u; r < 4u * P.ns; r = r + 1u) {
    var v = 0.0;
    if (i < P.nc) {
      let at = P.offSpC + r * P.nc + i;
      v = faceS[at];
      faceS[at] = 0.0;
    }
    phShared[lid.x] = v;
    workgroupBarrier();
    for (var stride = 128u; stride > 0u; stride = stride >> 1u) {
      if (lid.x < stride) { phShared[lid.x] = phShared[lid.x] + phShared[lid.x + stride]; }
      workgroupBarrier();
    }
    if (lid.x == 0u) { faceS[P.offSpB + r * P.spGroups + wid.x] = phShared[0]; }
    workgroupBarrier();
  }
}
)WGSL";

} // namespace openswmm::twoD::webgpu
