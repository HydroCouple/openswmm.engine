// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      ImplicitReachSolver.cpp
 * @brief     Standalone implicit finite-volume reach solver.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "ImplicitReachSolver.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace openswmm::fv {
namespace {
using Solver = ImplicitReachSolver;
using Pair = Solver::Pair;
using Block = Solver::Block;
namespace k = kernels;

/**
 * @brief Check both components of a local two-variable value.
 *
 * @param[in] p Pair whose components are tested without mutation.
 * @return True only when both doubles are finite.
 */
bool finite(const Pair& p) {
    return std::isfinite(p[0]) && std::isfinite(p[1]);
}
/**
 * @brief Multiply a row-major two-by-two block by a two-component column.
 *
 * @param[in] b Four row-major scalar coefficients.
 * @param[in] p Two-component column in matching units.
 * @return Value pair b*p; no finite check or mutation.
 */
Pair product(const Block& b, const Pair& p) {
    return {b[0] * p[0] + b[1] * p[1], b[2] * p[0] + b[3] * p[1]};
}
/**
 * @brief Multiply two row-major two-by-two blocks.
 *
 * @param[in] a Left block.
 * @param[in] b Right block.
 * @return Row-major product a*b; no finite check or mutation.
 */
Block product(const Block& a, const Block& b) {
    return {a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2],
            a[2] * b[1] + a[3] * b[3]};
}
/**
 * @brief Solve one finite two-by-two block with local partial pivoting.
 *
 * No cross-block pivoting, regularization or allocation is performed. The quality ratio is not a
 * condition-number estimate.
 *
 * @param[in] a Row-major coefficient block copied before elimination.
 * @param[in] b Finite RHS pair copied before row swaps.
 * @param[out] x Solution pair; untouched on early validation/pivot failure but may contain
 *     nonfinite values on a final arithmetic failure.
 * @param[out] quality Optional pivot/maximum-entry ratio; initialized to zero and populated when
 *     the second pivot is computed.
 * @return True if both scaled pivots exceed 1e-14 and the final solution is finite.
 */
bool blockSolve(Block a, Pair b, Pair& x, double* quality = nullptr) {
    if (quality)
        *quality = 0.0;
    double scale = 0.0;
    for (double v : a) {
        if (!std::isfinite(v))
            return false;
        scale = std::max(scale, std::fabs(v));
    }
    if (!(scale > 0.0) || !finite(b))
        return false;
    if (std::fabs(a[2]) > std::fabs(a[0])) {
        std::swap(a[0], a[2]);
        std::swap(a[1], a[3]);
        std::swap(b[0], b[1]);
    }
    if (std::fabs(a[0]) <= 1.e-14 * scale)
        return false;
    const double m = a[2] / a[0], pivot = a[3] - m * a[1];
    if (!std::isfinite(pivot))
        return false;
    if (quality)
        *quality = std::min(std::fabs(a[0]), std::fabs(pivot)) / scale;
    if (std::fabs(pivot) <= 1.e-14 * scale)
        return false;
    x[1] = (b[1] - m * b[0]) / pivot;
    x[0] = (b[0] - a[1] * x[1]) / a[0];
    return finite(x);
}

/**
 * @brief Validate the supported flat single-reach implicit experiment shape and old state.
 *
 * Rejects force mains, local losses, gates, culverts, non-flat beds and invalid
 * directional/cell-face maps. It assumes an internally coherent positive geometry closure; table
 * sample monotonicity/radius is not independently certified. No data is changed.
 *
 * @param[in] p Borrowed problem with live mesh; checks all accessed topology/geometry vector
 *     dimensions and fixed endpoint stages.
 * @param[in] dt Required finite positive timestep in seconds.
 * @return True for supported closed above-crown prismatic topology and finite in-bound old
 *     areas/discharges; false otherwise.
 */
bool valid(const Solver::Problem& p, double dt) {
    if (!p.mesh || !std::isfinite(dt) || !(dt > 0.0))
        return false;
    const auto& m = *p.mesh;
    const auto n = p.old_area.size();
    if (n < 2 || p.old_discharge.size() != n || m.cell_dx.size() != n || m.geom.size() != 1 ||
        m.conduit_cell_count.size() != 1 || m.conduit_cell_count[0] != static_cast<int>(n) ||
        m.conduit_cell_begin.size() != 1 || m.conduit_cell_begin[0] != 0 ||
        m.face_cl.size() != n + 1 || m.face_cr.size() != n + 1 || m.face_dir_l.size() != n + 1 ||
        m.face_dir_r.size() != n + 1 || m.face_zb.size() != n + 1 || m.cell_zb.size() != n ||
        m.face_node.size() != n + 1 || (!m.face_geom.empty() && m.face_geom.size() != n + 1) ||
        m.cell_face0.size() != n || m.cell_face1.size() != n || m.cell_side0.size() != n ||
        m.cell_side1.size() != n || (!m.face_gate.empty() && m.face_gate.size() != n + 1) ||
        (!m.face_culvert.empty() && m.face_culvert.size() != n + 1) || m.cell_geom.size() != n ||
        m.cell_dzdx.size() != n || m.cell_conduit.size() != n || m.conduit_loss_inlet.size() != 1 ||
        m.conduit_loss_outlet.size() != 1 || m.conduit_rough_factor.size() != 1 ||
        m.conduit_loss_inlet[0] != 0.0 || m.conduit_loss_outlet[0] != 0.0 ||
        !std::isfinite(m.conduit_rough_factor[0]) || m.conduit_rough_factor[0] < 0.0)
        return false;
    const auto& g = m.geom[0];
    if (g.is_open || !(g.t_slot > 0.0) || !(g.a_crown > 0.0) || !std::isfinite(p.lower_depth) ||
        !std::isfinite(p.upper_depth) || p.lower_depth < g.y_full ||
        !(p.upper_depth > p.lower_depth) || g.xs.type == static_cast<int>(XSectShape::FORCE_MAIN))
        return false;
    for (double h : {p.left_depth, p.right_depth})
        if (!std::isnan(h) && (!std::isfinite(h) || h < p.lower_depth || h > p.upper_depth))
            return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(m.cell_dx[i]) || !(m.cell_dx[i] > 0.0) || !std::isfinite(m.cell_zb[i]) ||
            m.cell_zb[i] != m.cell_zb[0] || m.cell_dzdx[i] != 0.0 || m.cell_geom[i] != 0 ||
            m.cell_conduit[i] != 0 || !std::isfinite(p.old_area[i]) ||
            !std::isfinite(p.old_discharge[i]))
            return false;
        const double h = k::depthOfArea(g, p.old_area[i]);
        if (!std::isfinite(h) || h < p.lower_depth || h > p.upper_depth)
            return false;
        const int left = static_cast<int>(i), right = left + 1;
        const bool normal = m.cell_face0[i] == left && m.cell_side0[i] == 1 &&
                            m.cell_face1[i] == right && m.cell_side1[i] == 0;
        const bool swapped = m.cell_face1[i] == left && m.cell_side1[i] == 1 &&
                             m.cell_face0[i] == right && m.cell_side0[i] == 0;
        if (!normal && !swapped)
            return false;
    }
    for (std::size_t f = 0; f <= n; ++f) {
        if (m.face_cl[f] != (f ? static_cast<int>(f) - 1 : -1) ||
            m.face_cr[f] != (f < n ? static_cast<int>(f) : -1) || m.face_dir_l[f] != 1 ||
            m.face_dir_r[f] != 1 || m.face_zb[f] != m.cell_zb[0])
            return false;
        if ((f > 0 && f < n && m.face_node[f] >= 0) ||
            (!m.face_geom.empty() && m.face_geom[f] != 0))
            return false;
        if ((!m.face_gate.empty() && m.face_gate[f] != 0) ||
            (!m.face_culvert.empty() && m.face_culvert[f] >= 0))
            return false;
    }
    return true;
}

/**
 * @brief Build a full-pressure scalar face state from trial depth/discharge.
 *
 * @param[in] g Valid initialized closed closure with strictly positive wet area.
 * @param[in] x Trial {depth ft, aggregate discharge ft3/s}.
 * @return Area/discharge/velocity/celerity/moment state with press=1; no dry guard on Q/A.
 */
k::FaceState state(const FvGeometry& g, const Pair& x) {
    const auto e = k::closureEval(g.closure_tbl, x[0]);
    k::FaceState s;
    s.a = e.a;
    s.q = x[1];
    s.u = s.q / s.a;
    s.c = k::celerity(s.a, e.t);
    s.i1 = e.i1;
    s.press = 1;
    return s;
}
/**
 * @brief Evaluate an interior or endpoint HLL face for a supported reach.
 *
 * Interior uses adjacent cells. NaN endpoint depth reflects discharge/velocity; prescribed depth
 * keeps adjacent velocity and marks the ghost press=2. No positivity/gate/local-loss cap is
 * applied.
 *
 * @param[in] p Validated problem with one live geometry and endpoint stage/wall descriptors.
 * @param[in] x Nonempty admissible trial depth/discharge pairs.
 * @param[in] f Face index in [0,x.size()], unchecked.
 * @return Native aggregate HLL flux positive left-to-right.
 */
k::FaceFlux face(const Solver::Problem& p, const std::vector<Pair>& x, std::size_t f) {
    const auto& g = p.mesh->geom[0];
    if (f > 0 && f < x.size())
        return k::riemannFlux(state(g, x[f - 1]), state(g, x[f]));
    const bool left = f == 0;
    const auto cell = state(g, x[left ? 0 : x.size() - 1]);
    const double h = left ? p.left_depth : p.right_depth;
    auto ghost = cell;
    if (std::isnan(h)) {
        ghost.q = -cell.q;
        ghost.u = -cell.u;
    } else {
        ghost = state(g, {h, k::areaOfDepth(g, h) * cell.u});
        ghost.press = 2;
    }
    return left ? k::riemannFlux(ghost, cell) : k::riemannFlux(cell, ghost);
}
/**
 * @brief Evaluate the signed Manning momentum sink at a trial reach cell.
 *
 * No dry, zero-radius or finite safeguard is applied here; the residual checks its resulting finite
 * value.
 *
 * @param[in] p Validated problem; one nonnegative native rough_factor and positive radius closure.
 * @param[in] x Wet trial depth ft and aggregate discharge ft3/s; positive closure area/radius
 *     required.
 * @return rough_factor*abs(Q/A)*Q/R^(4/3), in ft3/s2.
 */
double friction(const Solver::Problem& p, const Pair& x) {
    const auto& g = p.mesh->geom[0];
    const double r = k::hydRadOfDepth(g, x[0]), a = k::areaOfDepth(g, x[0]);
    return p.mesh->conduit_rough_factor[0] * std::fabs(x[1] / a) * x[1] / (r * std::cbrt(r));
}
/**
 * @brief Check trial size, finite values and configured depth bounds.
 *
 * @param[in] p Problem supplying expected cell count and lower/upper depths.
 * @param[in] x Trial {depth,discharge} pairs.
 * @return True if size matches old_area and every pair is finite with depth within inclusive
 *     bounds; discharge is otherwise unbounded.
 */
bool admissible(const Solver::Problem& p, const std::vector<Pair>& x) {
    if (x.size() != p.old_area.size())
        return false;
    for (const auto& v : x)
        if (!finite(v) || v[0] < p.lower_depth || v[0] > p.upper_depth)
            return false;
    return true;
}
} // namespace

bool ImplicitReachSolver::residual(const Problem& p, const std::vector<Pair>& x, double dt,
                                   std::vector<Pair>& output, std::vector<k::FaceFlux>* faces) {
    if (!valid(p, dt) || !admissible(p, x))
        return false;
    const auto n = x.size();
    std::vector<k::FaceFlux> flux(n + 1);
    for (std::size_t f = 0; f <= n; ++f) {
        flux[f] = face(p, x, f);
        if (!std::isfinite(flux[f].mass) || !std::isfinite(flux[f].mom))
            return false;
    }
    std::vector<Pair> r(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double dx = p.mesh->cell_dx[i];
        r[i] = {dx * (k::areaOfDepth(p.mesh->geom[0], x[i][0]) - p.old_area[i]) +
                    dt * (flux[i + 1].mass - flux[i].mass),
                dx * (x[i][1] - p.old_discharge[i]) +
                    dt * (flux[i + 1].mom - flux[i].mom + dx * friction(p, x[i]))};
        if (!finite(r[i]))
            return false;
    }
    output = std::move(r);
    if (faces)
        *faces = std::move(flux);
    return true;
}

bool ImplicitReachSolver::linearize(const Problem& p, const std::vector<Pair>& x, double dt,
                                    double step, Linearization& out) {
    if (!std::isfinite(step) || !(step > 0.0))
        return false;
    Linearization l;
    if (!residual(p, x, dt, l.residual))
        return false;
    const auto n = x.size();
    const auto& g = p.mesh->geom[0];
    l.lower.resize(n);
    l.diagonal.resize(n);
    l.upper.resize(n);
    l.row_scale.resize(n);
    double qscale = 0.0;
    for (double a : p.old_area) {
        const double h = k::depthOfArea(g, a), t = k::widthOfDepth(g, h);
        qscale = std::max(qscale, k::celerity(a, t) * t);
    }
    l.column_scale = {1.0, qscale};
    if (!(qscale > 0.0) || !std::isfinite(qscale))
        return false;
    for (std::size_t i = 0; i < n; ++i)
        l.row_scale[i] = {p.mesh->cell_dx[i] * k::widthOfDepth(g, k::depthOfArea(g, p.old_area[i])),
                          p.mesh->cell_dx[i] * qscale};
    auto trial = x;
    for (std::size_t j = 0; j < n; ++j)
        for (int col = 0; col < 2; ++col) {
            double lo = x[j][col] - step * std::max(1.0, std::fabs(x[j][col]));
            double hi = x[j][col] + step * std::max(1.0, std::fabs(x[j][col]));
            if (col == 0) {
                lo = std::max(lo, p.lower_depth);
                hi = std::min(hi, p.upper_depth);
            }
            if (!(hi > lo))
                return false;
            trial[j][col] = hi;
            const auto lp = face(p, trial, j), rp = face(p, trial, j + 1);
            const double fp = friction(p, trial[j]);
            trial[j][col] = lo;
            const auto lm = face(p, trial, j), rm = face(p, trial, j + 1);
            const double fm = friction(p, trial[j]);
            trial[j] = x[j];
            const Pair dl = {(lp.mass - lm.mass) / (hi - lo), (lp.mom - lm.mom) / (hi - lo)};
            const Pair dr = {(rp.mass - rm.mass) / (hi - lo), (rp.mom - rm.mom) / (hi - lo)};
            const double dx = p.mesh->cell_dx[j];
            for (int row = 0; row < 2; ++row) {
                const double scale = l.column_scale[col];
                l.diagonal[j][2 * row + col] =
                    dt * (dr[row] - dl[row]) * scale / l.row_scale[j][row];
                if (j > 0)
                    l.upper[j - 1][2 * row + col] = dt * dl[row] * scale / l.row_scale[j - 1][row];
                if (j + 1 < n)
                    l.lower[j + 1][2 * row + col] = -dt * dr[row] * scale / l.row_scale[j + 1][row];
            }
            l.diagonal[j][col == 0 ? 0 : 3] += dx * (col == 0 ? k::widthOfDepth(g, x[j][0]) : 1.0) *
                                               l.column_scale[col] / l.row_scale[j][col];
            l.diagonal[j][2 + col] +=
                dt * dx * (fp - fm) / (hi - lo) * l.column_scale[col] / l.row_scale[j][1];
        }
    for (std::size_t i = 0; i < n; ++i)
        for (int row = 0; row < 2; ++row) {
            l.residual[i][row] /= l.row_scale[i][row];
            if (!finite(l.residual[i]))
                return false;
        }
    out = std::move(l);
    return true;
}

bool ImplicitReachSolver::solveBlocks(const Linearization& l, const std::vector<Pair>& rhs,
                                      std::vector<Pair>& output, double& relative,
                                      BlockDiagnostics* diagnostics) {
    if (diagnostics)
        *diagnostics = {};
    const auto n = rhs.size();
    if (!n || l.lower.size() != n || l.diagonal.size() != n || l.upper.size() != n)
        return false;
    std::vector<Block> upper(n);
    std::vector<Pair> b(n), x(n);
    for (std::size_t i = 0; i < n; ++i) {
        auto d = l.diagonal[i];
        auto v = rhs[i];
        if (i) {
            const auto correction = product(l.lower[i], upper[i - 1]);
            const auto shift = product(l.lower[i], b[i - 1]);
            for (int k = 0; k < 4; ++k)
                d[k] -= correction[k];
            for (int k = 0; k < 2; ++k)
                v[k] -= shift[k];
        }
        double quality = 0.0;
        const bool safe = blockSolve(d, v, b[i], &quality);
        if (diagnostics)
            diagnostics->minimum_pivot_ratio = std::min(diagnostics->minimum_pivot_ratio, quality);
        if (!safe) {
            if (diagnostics)
                diagnostics->failed_block = static_cast<int>(i);
            return false;
        }
        if (i + 1 < n) {
            Pair first{}, second{};
            if (!blockSolve(d, {l.upper[i][0], l.upper[i][2]}, first) ||
                !blockSolve(d, {l.upper[i][1], l.upper[i][3]}, second))
                return false;
            upper[i] = {first[0], second[0], first[1], second[1]};
        }
    }
    x[n - 1] = b[n - 1];
    for (std::size_t i = n - 1; i-- > 0;) {
        const auto shift = product(upper[i], x[i + 1]);
        x[i] = {b[i][0] - shift[0], b[i][1] - shift[1]};
    }
    double norm = 0.0, error = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        auto a = product(l.diagonal[i], x[i]);
        if (i) {
            const auto v = product(l.lower[i], x[i - 1]);
            a[0] += v[0];
            a[1] += v[1];
        }
        if (i + 1 < n) {
            const auto v = product(l.upper[i], x[i + 1]);
            a[0] += v[0];
            a[1] += v[1];
        }
        for (int k = 0; k < 2; ++k) {
            norm = std::hypot(norm, rhs[i][k]);
            error = std::hypot(error, a[k] - rhs[i][k]);
        }
        if (!finite(x[i]) || !finite(a))
            return false;
    }
    relative = norm > 0.0 ? error / norm : error;
    if (!std::isfinite(relative) || error > 1.e-12 + 1.e-10 * norm)
        return false;
    output = std::move(x);
    return true;
}

ImplicitReachSolver::LinearResult ImplicitReachSolver::solveLinear(const Linearization& l,
                                                                   const std::vector<Pair>& rhs,
                                                                   const LinearControls& controls,
                                                                   std::vector<Pair>& output) {
    LinearResult result;
    const auto n = rhs.size();
    if (!n || l.lower.size() != n || l.diagonal.size() != n || l.upper.size() != n ||
        (controls.method != LinearMethod::StructuredThenGmres &&
         controls.method != LinearMethod::StructuredOnly &&
         controls.method != LinearMethod::GmresOnly) ||
        (controls.preconditioner != Preconditioner::BlockLine &&
         controls.preconditioner != Preconditioner::BlockJacobi))
        return result;
    for (std::size_t i = 0; i < n; ++i) {
        if (!finite(rhs[i]))
            return result;
        for (const auto* block : {&l.lower[i], &l.diagonal[i], &l.upper[i]})
            for (double value : *block)
                if (!std::isfinite(value))
                    return result;
    }
    if (controls.method != LinearMethod::GmresOnly) {
        if (solveBlocks(l, rhs, output, result.relative_residual, &result.direct)) {
            result.converged = true;
            return result;
        }
        if (controls.method == LinearMethod::StructuredOnly)
            return result;
        result.fallback = true;
    }
    result.used_gmres = true;
    const bool line = controls.preconditioner == Preconditioner::BlockLine;
    // Inverse blocks + flat rhs/answer + reconstructed answer; a line also
    // retains modified upper blocks and one forward-substitution vector.
    const auto bytes_per_block =
        sizeof(Block) + 3 * sizeof(Pair) + (line ? sizeof(Block) + sizeof(Pair) : 0);
    if (n > controls.gmres.workspace_limit_bytes / bytes_per_block ||
        n > std::numeric_limits<std::size_t>::max() / 2) {
        result.gmres.status = NativeGmres::Status::MemoryLimit;
        return result;
    }
    result.wrapper_bytes = n * bytes_per_block;
    auto gmres_controls = controls.gmres;
    gmres_controls.workspace_limit_bytes -= result.wrapper_bytes;
    std::vector<Block> inverse(n);
    std::vector<Block> upper(line ? n : 0);
    std::vector<Pair> forward(line ? n : 0);
    std::vector<double> flat_rhs(2 * n), flat_answer;
    for (std::size_t i = 0; i < n; ++i) {
        auto d = l.diagonal[i];
        if (line && i) {
            const auto correction = product(l.lower[i], upper[i - 1]);
            for (int k = 0; k < 4; ++k)
                d[k] -= correction[k];
        }
        Pair a{}, b{};
        auto invert = [&](const Block& value) {
            return blockSolve(value, {1, 0}, a) && blockSolve(value, {0, 1}, b);
        };
        bool safe = invert(d);
        if (!safe && line) {
            double scale = 1.0;
            for (double v : d) {
                if (!std::isfinite(v)) {
                    result.gmres.status = NativeGmres::Status::PreconditionerFailure;
                    return result;
                }
                scale = std::max(scale, std::fabs(v));
            }
            // Fixed numeric preconditioner repair, not a diagonal change to A.
            // Start at 1e-2 of the block scale to limit cancellation in the
            // fixed-factor application, instead of an epsilon-sized pivot.
            for (double fraction : {1.e-2, 1.e-1, 1.0}) {
                auto shifted = d;
                const double shift = fraction * scale;
                shifted[0] += shift;
                shifted[3] += shift;
                if (invert(shifted)) {
                    safe = true;
                    ++result.shifted_preconditioner_blocks;
                    result.maximum_preconditioner_shift =
                        std::max(result.maximum_preconditioner_shift, shift);
                    break;
                }
            }
            if (!safe) {
                result.gmres.status = NativeGmres::Status::PreconditionerFailure;
                return result;
            }
        }
        if (safe)
            inverse[i] = {a[0], b[0], a[1], b[1]};
        else {
            inverse[i] = {1, 0, 0, 1};
            ++result.identity_preconditioner_blocks;
        }
        if (line && i + 1 < n)
            upper[i] = product(inverse[i], l.upper[i]);
        flat_rhs[2 * i] = rhs[i][0];
        flat_rhs[2 * i + 1] = rhs[i][1];
    }
    auto matrix = [&](const double* x, double* y) {
        for (std::size_t i = 0; i < n; ++i) {
            auto v = product(l.diagonal[i], Pair{x[2 * i], x[2 * i + 1]});
            if (i) {
                const auto q = product(l.lower[i], Pair{x[2 * i - 2], x[2 * i - 1]});
                v[0] += q[0];
                v[1] += q[1];
            }
            if (i + 1 < n) {
                const auto q = product(l.upper[i], Pair{x[2 * i + 2], x[2 * i + 3]});
                v[0] += q[0];
                v[1] += q[1];
            }
            y[2 * i] = v[0];
            y[2 * i + 1] = v[1];
        }
        return true;
    };
    auto precondition = [&](const double* x, double* y) {
        if (line) {
            for (std::size_t i = 0; i < n; ++i) {
                Pair value{x[2 * i], x[2 * i + 1]};
                if (i) {
                    const auto shift = product(l.lower[i], forward[i - 1]);
                    value[0] -= shift[0];
                    value[1] -= shift[1];
                }
                forward[i] = product(inverse[i], value);
            }
            for (std::size_t i = n; i-- > 0;) {
                auto value = forward[i];
                if (i + 1 < n) {
                    const auto shift = product(upper[i], Pair{y[2 * i + 2], y[2 * i + 3]});
                    value[0] -= shift[0];
                    value[1] -= shift[1];
                }
                y[2 * i] = value[0];
                y[2 * i + 1] = value[1];
            }
            return true;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const auto v = product(inverse[i], Pair{x[2 * i], x[2 * i + 1]});
            y[2 * i] = v[0];
            y[2 * i + 1] = v[1];
        }
        return true;
    };
    NativeGmres solver;
    result.gmres = solver.solve(flat_rhs, matrix, precondition, gmres_controls, flat_answer);
    if (!result.gmres.converged())
        return result;
    double norm = 0;
    for (const auto& v : rhs)
        norm = std::hypot(std::hypot(norm, v[0]), v[1]);
    result.relative_residual =
        norm > 0.0 ? result.gmres.true_residual / norm : result.gmres.true_residual;
    std::vector<Pair> answer(n);
    for (std::size_t i = 0; i < n; ++i)
        answer[i] = {flat_answer[2 * i], flat_answer[2 * i + 1]};
    output = std::move(answer);
    result.converged = true;
    return result;
}

ImplicitReachSolver::Result ImplicitReachSolver::solve(const Problem& p, double dt,
                                                       const Controls& controls, Endpoint& output) {
    Result result;
    if (!valid(p, dt) || controls.max_newton < 0 || controls.max_backtracks < 0 ||
        !std::isfinite(controls.difference_step) || controls.difference_step <= 0.0)
        return result;
    for (double v : {controls.absolute_volume, controls.relative_volume, controls.absolute_momentum,
                     controls.relative_momentum})
        if (!std::isfinite(v) || v < 0.0)
            return result;
    if (controls.absolute_volume + controls.relative_volume <= 0 ||
        controls.absolute_momentum + controls.relative_momentum <= 0)
        return result;
    const auto n = p.old_area.size();
    const auto& g = p.mesh->geom[0];
    std::vector<Pair> x(n), r, trial_r, budgets(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = {k::depthOfArea(g, p.old_area[i]), p.old_discharge[i]};
        budgets[i] = {controls.absolute_volume +
                          controls.relative_volume *
                              std::max(1.0, p.mesh->cell_dx[i] * p.old_area[i]),
                      controls.absolute_momentum +
                          controls.relative_momentum *
                              std::max(1.0, p.mesh->cell_dx[i] * std::fabs(p.old_discharge[i]))};
        if (!finite(budgets[i]) || !(budgets[i][0] > 0.0) || !(budgets[i][1] > 0.0))
            return result;
    }
    auto norm = [&](const std::vector<Pair>& value) {
        double maximum = 0.0;
        for (std::size_t i = 0; i < n; ++i)
            for (int k = 0; k < 2; ++k)
                maximum = std::max(maximum, std::fabs(value[i][k]) / budgets[i][k]);
        return maximum;
    };
    for (int it = 0; it <= controls.max_newton; ++it) {
        if (!residual(p, x, dt, r)) {
            result.status = Status::NonFinite;
            return result;
        }
        result.scaled_residual = norm(r);
        result.max_volume_residual = 0.0;
        result.max_momentum_residual = 0.0;
        for (const auto& v : r) {
            result.max_volume_residual = std::max(result.max_volume_residual, std::fabs(v[0]));
            result.max_momentum_residual = std::max(result.max_momentum_residual, std::fabs(v[1]));
        }
        if (result.scaled_residual <= 1.0) {
            Endpoint endpoint;
            if (!residual(p, x, dt, r, &endpoint.faces)) {
                result.status = Status::NonFinite;
                return result;
            }
            for (const auto& v : x) {
                endpoint.area.push_back(k::areaOfDepth(g, v[0]));
                endpoint.discharge.push_back(v[1]);
                endpoint.depth.push_back(v[0]);
            }
            output = std::move(endpoint);
            result.status = Status::Converged;
            return result;
        }
        if (it == controls.max_newton) {
            result.status = Status::IterationLimit;
            return result;
        }
        Linearization l;
        if (!linearize(p, x, dt, controls.difference_step, l)) {
            result.status = Status::NonFinite;
            return result;
        }
        auto rhs = l.residual;
        for (auto& v : rhs) {
            v[0] = -v[0];
            v[1] = -v[1];
        }
        std::vector<Pair> delta;
        auto linear_controls = controls.linear;
        // A generic scaled absolute tolerance can accept a zero correction
        // while a physical momentum/volume row is still outside its budget.
        // Tighten the absolute linear floor to a tenth of the smallest current
        // normalized nonlinear budget. The actual nonlinear gates are unchanged.
        double absolute_floor = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < n; ++i)
            for (int k = 0; k < 2; ++k)
                absolute_floor = std::min(absolute_floor, .1 * budgets[i][k] / l.row_scale[i][k]);
        linear_controls.gmres.absolute_tolerance =
            std::min(linear_controls.gmres.absolute_tolerance, absolute_floor);
        result.last_linear = solveLinear(l, rhs, linear_controls, delta);
        result.linear_relative_residual = result.last_linear.relative_residual;
        result.gmres_iterations += result.last_linear.gmres.iterations;
        result.gmres_solves += result.last_linear.used_gmres;
        result.linear_fallbacks += result.last_linear.fallback;
        result.max_gmres_workspace_bytes =
            std::max(result.max_gmres_workspace_bytes,
                     result.last_linear.wrapper_bytes + result.last_linear.gmres.workspace_bytes);
        if (!result.last_linear.converged) {
            result.status = Status::LinearFailure;
            return result;
        }
        auto trial = x;
        bool accepted = false;
        double fraction = 1.0;
        for (int back = 0; back <= controls.max_backtracks; ++back) {
            for (std::size_t i = 0; i < n; ++i)
                for (int k = 0; k < 2; ++k)
                    trial[i][k] = x[i][k] + fraction * l.column_scale[k] * delta[i][k];
            if (residual(p, trial, dt, trial_r) &&
                (norm(trial_r) <= 1.0 ||
                 norm(trial_r) < (1.0 - 1.e-4 * fraction) * result.scaled_residual)) {
                x.swap(trial);
                accepted = true;
                break;
            }
            fraction *= 0.5;
            ++result.backtracks;
        }
        if (!accepted) {
            result.status = Status::LineSearchFailed;
            return result;
        }
        ++result.newton_iterations;
    }
    return result;
}
} // namespace openswmm::fv
