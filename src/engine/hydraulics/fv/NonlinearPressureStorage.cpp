// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "NonlinearPressureStorage.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace openswmm::fv {

NonlinearPressureStorage::Result NonlinearPressureStorage::solve(
    const std::vector<Row>& rows, const std::vector<Edge>& edges, double dt,
    const Controls& c, std::vector<double>& output_heads) {
    Result result;
    const auto n = rows.size();
    auto positive = [](double x) { return std::isfinite(x) && x > 0.0; };
    if (n == 0 || n > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !positive(dt) || c.max_newton < 0 || c.max_linear < 0 || c.max_backtracks < 0 ||
        !positive(c.absolute_volume) || !std::isfinite(c.relative_volume) ||
        c.relative_volume < 0.0 || !positive(c.linear_relative) ||
        c.linear_relative >= 1.0 || !positive(c.linear_absolute)) return result;

    head_.resize(n); trial_.resize(n); residual_.resize(n); trial_residual_.resize(n);
    budget_.resize(n); diag_.resize(n); delta_.resize(n); r_.resize(n);
    z_.resize(n); p_.resize(n); ap_.resize(n); parent_.resize(n); anchored_.resize(n);
    weight_.resize(edges.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto& row = rows[i];
        if (!std::isfinite(row.bed) || !std::isfinite(row.old_head) ||
            !std::isfinite(row.source) || !std::isfinite(row.lower_head) ||
            !std::isfinite(row.upper_head) || row.lower_head > row.old_head ||
            row.old_head > row.upper_head) return result;
        if (row.geometry && (row.geometry->is_open || !positive(row.length) ||
            !positive(row.geometry->t_slot) || !positive(row.geometry->y_full) ||
            row.lower_head < row.bed || !std::isfinite(row.old_area) ||
            row.old_area < 0.0)) return result;
        head_[i] = row.old_head;
        const double old_volume = row.geometry ? row.length * row.old_area : 0.0;
        budget_[i] = c.absolute_volume + c.relative_volume *
            std::max({1.0, std::fabs(old_volume), dt * std::fabs(row.source)});
        if (!std::isfinite(old_volume) || !positive(budget_[i])) return result;
    }
    for (std::size_t e = 0; e < edges.size(); ++e) {
        const auto& edge = edges[e];
        if (edge.left < 0 || static_cast<std::size_t>(edge.left) >= n ||
            edge.right < -1 || (edge.right >= 0 && static_cast<std::size_t>(edge.right) >= n) ||
            edge.left == edge.right || !positive(edge.conductance) ||
            (edge.right == -1 && !std::isfinite(edge.boundary_head))) return result;
        weight_[e] = dt * edge.conductance;
        if (!positive(weight_[e])) return result;
    }

    auto evaluate = [&](const std::vector<double>& h, std::vector<double>& r) {
        ++result.residual_evaluations;
        for (std::size_t i = 0; i < n; ++i) {
            const auto& row = rows[i];
            r[i] = -dt * row.source;
            if (row.geometry) {
                const auto& g = *row.geometry;
                r[i] += row.length * (kernels::areaOfDepth(g, h[i] - row.bed) - row.old_area);
            }
        }
        for (std::size_t e = 0; e < edges.size(); ++e) {
            const auto& edge = edges[e];
            const double other = edge.right < 0 ? edge.boundary_head : h[edge.right];
            const double transfer = weight_[e] * (h[edge.left] - other);
            r[edge.left] += transfer;
            if (edge.right >= 0) r[edge.right] -= transfer;
        }
        double scaled = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!std::isfinite(r[i])) return std::numeric_limits<double>::infinity();
            scaled = std::max(scaled, std::fabs(r[i]) / budget_[i]);
        }
        return scaled;
    };
    auto measure = [&] {
        result.max_volume_residual = 0.0;
        result.sum_absolute_residual = 0.0;
        for (double value : residual_) {
            if (!std::isfinite(value)) {
                result.max_volume_residual = std::numeric_limits<double>::infinity();
                result.sum_absolute_residual = result.max_volume_residual;
                return;
            }
            result.max_volume_residual = std::max(result.max_volume_residual, std::fabs(value));
            result.sum_absolute_residual += std::fabs(value);
        }
    };
    // Matrix product for the current exact storage tangent and frozen edges.
    auto matvec = [&](const std::vector<double>& x, std::vector<double>& out) {
        for (std::size_t i = 0; i < n; ++i) out[i] = diag_[i] * x[i];
        for (std::size_t e = 0; e < edges.size(); ++e) {
            const auto& edge = edges[e];
            if (edge.right < 0) continue;
            out[edge.left] -= weight_[e] * x[edge.right];
            out[edge.right] -= weight_[e] * x[edge.left];
        }
    };
    auto norm = [](const std::vector<double>& x) {
        double value = 0.0;
        for (double v : x) value = std::hypot(value, v);
        return value;
    };
    auto root = [&](int i) {
        while (parent_[i] != i) {
            parent_[i] = parent_[parent_[i]];
            i = parent_[i];
        }
        return i;
    };

    result.scaled_residual = evaluate(head_, residual_);
    for (int it = 0;; ++it) {
        measure();
        if (!std::isfinite(result.scaled_residual)) {
            result.status = Status::NonFinite;
            return result;
        }
        // Check Jacobian eligibility even for an initially zero residual:
        // an unanchored algebraic component has no unique head solution.
        for (std::size_t i = 0; i < n; ++i) {
            const auto& row = rows[i];
            diag_[i] = row.geometry ? row.length *
                kernels::widthOfDepth(*row.geometry, head_[i] - row.bed) : 0.0;
            if (!std::isfinite(diag_[i]) || diag_[i] < 0.0) {
                result.status = Status::NonFinite;
                return result;
            }
            anchored_[i] = diag_[i] > 0.0;
            parent_[i] = static_cast<int>(i);
        }
        for (std::size_t e = 0; e < edges.size(); ++e) {
            const auto& edge = edges[e];
            diag_[edge.left] += weight_[e];
            if (edge.right >= 0) {
                diag_[edge.right] += weight_[e];
                const int a = root(edge.left), b = root(edge.right);
                parent_[a] = b;
            } else anchored_[edge.left] = 1;
        }
        for (std::size_t i = 0; i < n; ++i)
            if (anchored_[i]) anchored_[root(static_cast<int>(i))] = 1;
        for (std::size_t i = 0; i < n; ++i) {
            if (!anchored_[root(static_cast<int>(i))]) {
                result.status = Status::UnanchoredComponent;
                return result;
            }
            if (!positive(diag_[i])) {
                result.status = Status::NonFinite;
                return result;
            }
        }
        if (result.scaled_residual <= 1.0) {
            // Re-evaluate the unchanged fine-grid equations at the endpoint.
            result.scaled_residual = evaluate(head_, residual_);
            measure();
            if (result.scaled_residual <= 1.0) {
                output_heads = head_;
                result.status = Status::Converged;
            } else result.status = Status::NonFinite;
            return result;
        }
        if (it == c.max_newton) {
            result.status = Status::NonlinearIterationLimit;
            return result;
        }

        // Native Jacobi-PCG. The positive storage/anchor check above plus
        // positive edges proves SPD for this restricted Jacobian only.
        std::fill(delta_.begin(), delta_.end(), 0.0);
        double rz = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            r_[i] = -residual_[i];
            z_[i] = r_[i] / diag_[i];
            p_[i] = z_[i];
            rz += r_[i] * z_[i];
        }
        result.linear_tolerance = std::max(c.linear_absolute, c.linear_relative * norm(r_));
        Status linear_status = Status::LinearIterationLimit;
        for (int k = 0;; ++k) {
            const double recursive_norm = norm(r_);
            if (!std::isfinite(recursive_norm)) {
                linear_status = Status::NonFinite;
                break;
            }
            if (recursive_norm <= result.linear_tolerance) {
                linear_status = Status::Converged;
                break;
            }
            if (k == c.max_linear) break;
            matvec(p_, ap_);
            double curvature = 0.0;
            for (std::size_t i = 0; i < n; ++i) curvature += p_[i] * ap_[i];
            if (!positive(curvature) || !positive(rz)) {
                linear_status = Status::LinearBreakdown;
                break;
            }
            const double alpha = rz / curvature;
            for (std::size_t i = 0; i < n; ++i) {
                delta_[i] += alpha * p_[i];
                r_[i] -= alpha * ap_[i];
                z_[i] = r_[i] / diag_[i];
            }
            ++result.linear_iterations;
            double next_rz = 0.0;
            for (std::size_t i = 0; i < n; ++i) next_rz += r_[i] * z_[i];
            const double beta = next_rz / rz;
            for (std::size_t i = 0; i < n; ++i) p_[i] = z_[i] + beta * p_[i];
            rz = next_rz;
        }
        matvec(delta_, ap_);
        for (std::size_t i = 0; i < n; ++i) ap_[i] = -residual_[i] - ap_[i];
        result.linear_residual = norm(ap_);
        if (!std::isfinite(result.linear_residual)) linear_status = Status::NonFinite;
        else if (linear_status == Status::Converged &&
                 result.linear_residual > result.linear_tolerance)
            linear_status = Status::LinearResidualFailure;
        if (linear_status != Status::Converged) {
            result.status = linear_status;
            return result;
        }

        double step = 1.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!std::isfinite(delta_[i])) {
                result.status = Status::NonFinite;
                return result;
            }
            if (delta_[i] < 0.0)
                step = std::min(step, 0.99 * (head_[i] - rows[i].lower_head) / -delta_[i]);
            else if (delta_[i] > 0.0)
                step = std::min(step, 0.99 * (rows[i].upper_head - head_[i]) / delta_[i]);
        }
        bool accepted = false;
        for (int back = 0; back <= c.max_backtracks && step > 0.0; ++back) {
            bool admissible = true;
            for (std::size_t i = 0; i < n; ++i) {
                trial_[i] = head_[i] + step * delta_[i];
                admissible = admissible && std::isfinite(trial_[i]) &&
                    trial_[i] >= rows[i].lower_head && trial_[i] <= rows[i].upper_head;
            }
            const double scaled = admissible ? evaluate(trial_, trial_residual_) :
                std::numeric_limits<double>::infinity();
            if (std::isfinite(scaled) && (scaled <= 1.0 ||
                scaled <= (1.0 - 1.e-4 * step) * result.scaled_residual)) {
                head_.swap(trial_);
                residual_.swap(trial_residual_);
                result.scaled_residual = scaled;
                accepted = true;
                ++result.newton_iterations;
                break;
            }
            if (back < c.max_backtracks) {
                step *= 0.5;
                ++result.backtracks;
            }
        }
        if (!accepted) {
            result.status = Status::LineSearchFailed;
            return result;
        }
    }
}

} // namespace openswmm::fv
