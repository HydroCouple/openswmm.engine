// SPDX-License-Identifier: Apache-2.0

/**
 * @file      RichardsColumn.cpp
 * @brief     Richards-equation soil column state and numerical integration.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "core/ProcessPerf.hpp"
#include "RichardsColumn.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <exception>
#include "core/ThreadInfo.hpp"
#include <limits>
#include <map>
#include <numeric>
#include <set>

namespace openswmm::richards {
bool valid(const Material& p, double ts) {
    return std::isfinite(ts) && ts > 0 && ts <= 1 && std::isfinite(p.theta_r) && p.theta_r >= 0 &&
           p.theta_r < ts && std::isfinite(p.alpha) && p.alpha > 0 && std::isfinite(p.n) &&
           p.n > 1 && std::isfinite(p.l) && p.l >= 0 && std::isfinite(p.specific_storage) &&
           p.specific_storage > 0;
}
bool valid(const Options& o) {
    return o.cells_per_layer >= 1 && o.cells_per_layer <= 256 && std::isfinite(o.atol) &&
           o.atol > 0 && std::isfinite(o.rtol) && o.rtol > 0 && std::isfinite(o.max_step) &&
           o.max_step > 0;
}
double theta(const Material& p, double ts, double h) {
    if (h >= 0)
        return ts;
    const double m = 1 - 1 / p.n;
    return p.theta_r + (ts - p.theta_r) * std::pow(1 + std::pow(-p.alpha * h, p.n), -m);
}
double storage(const Material& p, double ts, double h) {
    return theta(p, ts, h) + p.specific_storage * std::max(0.0, h);
}
double pressure(const Material& p, double ts, double w) {
    if (w >= ts)
        return (w - ts) / p.specific_storage;
    if (w <= p.theta_r)
        return -std::numeric_limits<double>::infinity();
    const double se = (w - p.theta_r) / (ts - p.theta_r), m = 1 - 1 / p.n;
    return -std::pow(std::expm1(-std::log(se) / m), 1 / p.n) / p.alpha;
}
double conductivity(const Material& p, double Ks, double h) {
    if (h >= 0)
        return Ks;
    const double m = 1 - 1 / p.n;
    const double se = std::pow(1 + std::pow(-p.alpha * h, p.n), -m);
    const double term = -std::expm1(m * std::log1p(-std::pow(se, 1 / m)));
    return Ks * std::pow(se, p.l) * term * term;
}
namespace {
struct BoundaryRates {
    std::vector<double> out, in;
};
/** @brief Convert a remaining directional volume reservation to a safe step rate.
 * @param limit Nonnegative finite m3 reservation, or positive infinity for unbounded.
 * @param spent Nonnegative volume m3 already booked against that direction.
 * @param step Proposed finite step seconds; nonpositive returns zero for finite limits.
 * @return Nonnegative m3/s, rounded toward zero until spent+step*rate<=limit. An
 * unbounded limit returns positive infinity before checking step/spent. No input
 * validation is added here; advance() qualifies column reservations. */
double reservedRate(double limit, double spent, double step) {
    if (!std::isfinite(limit))
        return std::numeric_limits<double>::infinity();
    if (step <= 0 || spent >= limit)
        return 0;
    double rate = std::min((limit - spent) / step, std::numeric_limits<double>::max());
    // Round toward the budget, using the same multiply/add as the receipt.
    while (spent + step * rate > limit)
        rate = std::nextafter(rate, 0.0);
    return rate;
}
template<bool Packed> struct Batch {
    int n, lanes;
    std::vector<Column*> columns;
    std::vector<double> y, volume, lower_bound;
    std::vector<Report*> reports;
    std::vector<double> head, K, f, fp, residual, lo, di, up, delta, perturbed, epsilon;
    std::vector<uint8_t> done;
    enum Coefficient { Area, Z, Dz, ThetaS, Ks, Wilting, PotentialEt,
                       ThetaR, Alpha, N, L, SpecificStorage, CoefficientCount };
    std::vector<double> coefficients;
    /** @brief Read private cell-major lanes, or retain the borrowed small-batch view.
     * Packed coefficients are frozen by construction for this advance only. Returning
     * a value in the packed case leaves the numerical expressions and material API
     * unchanged; the compiler can keep just the fields used by each caller. */
    decltype(auto) cellAt(int k, int a) const {
        if constexpr (Packed) {
            const auto i = static_cast<std::size_t>(k * lanes + a);
            const auto field = [&](Coefficient p) { return coefficients[p * y.size() + i]; };
            return Cell{volume[i], field(Area), field(Z), field(Dz), field(ThetaS), field(Ks),
                        field(Wilting), field(PotentialEt),
                        {field(ThetaR), field(Alpha), field(N), field(L), field(SpecificStorage)}};
        } else {
            return static_cast<const Cell&>(columns[a]->cells[k]);
        }
    }
    /** @brief Pack borrowed compatible columns and report targets into cell-major lanes.
     * @param c Nonempty vector of nonnull borrowed columns with the same cell count,
     * validated geometry/water, and counts/products fitting int; copied pointer vector
     * is moved into storage, column data copied into owned scratch.
     * @param r One nonnull borrowed report pointer per lane, valid throughout this batch.
     * Computes residual-volume lower bounds; no column water is changed until run()
     * finishes. Allocation failures propagate; no pointer ownership transfers. */
    Batch(std::vector<Column*> c, std::vector<Report*> r)
        : n(c.front()->cells.size()), lanes(c.size()), columns(std::move(c)),
          reports(std::move(r)) {
        y.resize(n * lanes);
        volume.resize(y.size());
        lower_bound.resize(y.size());
        for (auto* scratch : {&head, &K, &f, &fp, &residual, &lo, &di, &up, &delta,
                              &perturbed, &epsilon})
            scratch->resize(y.size());
        done.resize(lanes);
        if constexpr (Packed)
            coefficients.resize(CoefficientCount * y.size());
        for (int k = 0; k < n; ++k)
            for (int a = 0; a < lanes; ++a) {
                const auto& cell = columns[a]->cells[k];
                const int i = k * lanes + a;
                y[i] = columns[a]->water[k];
                volume[i] = cell.volume;
                if constexpr (Packed) {
                    const double values[] = {cell.area, cell.z, cell.dz, cell.theta_s, cell.Ks,
                        cell.wilting, cell.potential_et, cell.material.theta_r, cell.material.alpha,
                        cell.material.n, cell.material.l, cell.material.specific_storage};
                    for (int p = 0; p < CoefficientCount; ++p)
                        coefficients[p * y.size() + i] = values[p];
                }
                lower_bound[i] = k ? cell.material.theta_r * cell.volume : 0.0;
            }
    }
    // Flux positive downward. Surface wet/dry transition is continuous over
    // 1 micrometre of ponding; no water is discarded at that transition.
    /** @brief Evaluate conservative intercell and boundary water rates on packed scratch.
     * @param w Valid cell-major/lane-minor stored volumes m3, size n*lanes.
     * @param[out] f Reassigned same-sized derivatives m3/s; must not alias w.
     * @param[out] faces Optional distinct writable packed rates, positive downward;
     * last cell stores signed bottom rate. Reassigned to n*lanes elements.
     * @param[out] et Optional distinct writable packed evaporation sink m3/s, reassigned.
     * @param inactive Optional lane mask at least lanes long; true skips work/counters.
     * @param rates Optional lane-sized nonnegative out/in reservation caps m3/s.
     * @param[out] limited Optional lane-sized flags, reset to zero; +1/-1 identifies a
     * bottom out/in cap applied at that evaluation endpoint.
     * Updates active reports' RHS counters only, not column water or accepted receipts.
     * Surface wet/dry stress transitions over one micrometre; internal faces conserve
     * water. Applies ET stress, supplied surface inflow, selected bottom conductance
     * and factor once, then reservation rate caps. Caller owns validity of all packed
     * sizes/materials; intermediate nonfinite results are handled by the solver later. */
    void rhs(const std::vector<double>& w, std::vector<double>& f,
             std::vector<double>* faces = nullptr, std::vector<double>* et = nullptr,
             const std::vector<uint8_t>* inactive = nullptr, const BoundaryRates* rates = nullptr,
             std::vector<int>* limited = nullptr) {
        f.assign(w.size(), 0.0);
        head.assign(w.size(), 0.0);
        K.assign(w.size(), 0.0);
        if (faces)
            faces->assign(n * lanes, 0.0);
        if (et)
            et->assign(w.size(), 0.0);
        if (limited)
            limited->assign(lanes, 0);
        for (int a = 0; a < lanes; ++a)
            if (!inactive || !(*inactive)[a])
                ++reports[a]->rhs;
        for (int a = 0; a < lanes; ++a)
            if ((!inactive || !(*inactive)[a]) && columns[a]->inflow > 0)
                f[a] += columns[a]->inflow;
        for (int k = 0; k < n; ++k)
            for (int a = 0; a < lanes; ++a) {
                if (inactive && (*inactive)[a])
                    continue;
                const int i = k * lanes + a;
                const auto& c = cellAt(k, a);
                head[i] = c.z + (k ? pressure(c.material, c.theta_s, w[i] / c.volume)
                                   : std::max(0.0, w[i]) / (c.area * c.theta_s));
                K[i] = k ? conductivity(c.material, c.Ks, head[i] - c.z) : 0.0;
                double stress = k ? std::clamp((w[i] / c.volume - c.wilting) /
                                                   std::max(c.theta_s - c.wilting, 1.e-12),
                                               0.0, 1.0)
                                  : std::clamp(w[i] / (c.area * c.theta_s * 1.e-6), 0.0, 1.0);
                const double e = c.potential_et * stress;
                f[i] -= e;
                if (et)
                    (*et)[i] = e;
            }
        for (int k = 0; k < n - 1; ++k)
            for (int a = 0; a < lanes; ++a) {
                if (inactive && (*inactive)[a])
                    continue;
                const int i = k * lanes + a, j = i + lanes;
                const auto& c = cellAt(k, a);
                const auto& d = cellAt(k + 1, a);
                const double area = std::min(c.area, d.area);
                double conductance = 0.0;
                // Arithmetic conductivity within a material follows openRE
                // (Ireson et al. 2023, Eq. 11/14). Using the dry cell's K alone
                // at a ponded boundary can numerically lock the wetting front.
                if (k == 0)
                    conductance = area * (d.Ks + K[j]) / d.dz;
                else if (c.material.alpha == d.material.alpha && c.material.n == d.material.n &&
                         c.material.theta_r == d.material.theta_r && c.material.l == d.material.l &&
                         c.Ks == d.Ks)
                    conductance =
                        area * (c.dz * K[j] + d.dz * K[i]) / (c.dz + d.dz) / (0.5 * (c.dz + d.dz));
                else if (K[i] > 0 && K[j] > 0)
                    conductance = area / (0.5 * c.dz / K[i] + 0.5 * d.dz / K[j]);
                double q = conductance * (head[i] - head[j]);
                if (k == 0 && q > 0)
                    q *= std::clamp(w[i] / (c.area * c.theta_s * 1.e-6), 0.0, 1.0);
                f[i] -= q;
                f[j] += q;
                if (faces)
                    (*faces)[i] = q;
            }
        for (int a = 0; a < lanes; ++a) {
            if (inactive && (*inactive)[a])
                continue;
            const int i = (n - 1) * lanes + a;
            const auto& col = *columns[a];
            const auto& c = cellAt(n - 1, a);
            const double area = col.bottom_area > 0 ? col.bottom_area : c.area;
            double q = 0.0;
            if (col.bottom == Bottom::FreeDrainage)
                q = std::min(K[i], col.bottom_K) * area;
            else if (col.bottom == Bottom::Head && K[i] > 0 && col.bottom_K > 0) {
                const double resistance = 0.5 * c.dz / K[i] + col.bottom_distance / col.bottom_K;
                q = area * (head[i] - col.bottom_head) / resistance;
            }
            q *= col.bottom_factor; // once, on the whole series conductance
            if (rates) {
                if (q > rates->out[a]) {
                    q = rates->out[a];
                    if (limited)
                        (*limited)[a] = 1;
                } else if (q < -rates->in[a]) {
                    q = -rates->in[a];
                    if (limited)
                        (*limited)[a] = -1;
                }
            }
            f[i] -= q;
            if (faces)
                (*faces)[i] = q;
        }
    }
    /** @brief Solve one lane-wise backward-Euler stage into private packed scratch.
     * @param start Initial packed water m3, n*lanes valid elements.
     * @param steps Nonnegative finite per-lane seconds; zero lanes succeed unchanged.
     * @param[out] out Reassigned solution/last trial iterate, distinct from start.
     * @param rates Optional lane-sized directional bottom caps m3/s.
     * @param[out] success One success flag per lane; false retains a last trial iterate which must
     * not be accepted. Uses up to 35 Newton iterations, three-color finite-difference
     * tridiagonal Jacobians and lower-bound damping. Updates report RHS/Newton counts;
     * does not write column water, reservations or accepted flux ledgers. Allocation
     * exceptions propagate. Callers decide retry/rollback and validate error estimates. */
    void implicit(const std::vector<double>& start, const std::vector<double>& steps,
                  std::vector<double>& out, std::vector<uint8_t>& success,
                  const BoundaryRates* rates = nullptr) {
        out = start;
        done.assign(lanes, false);
        success.assign(lanes, false);
        std::fill(residual.begin(), residual.end(), 0.0);
        for (int a = 0; a < lanes; ++a)
            if (steps[a] == 0)
                done[a] = success[a] = true;
        for (int it = 0; it < 35; ++it) {
            rhs(out, f, nullptr, nullptr, &done, rates);
            bool all = true;
            for (int a = 0; a < lanes; ++a)
                if (!done[a]) {
                    ++reports[a]->newton;
                    double norm = 0;
                    for (int k = 0; k < n; ++k) {
                        int i = k * lanes + a;
                        residual[i] = out[i] - start[i] - steps[a] * f[i];
                        if (!std::isfinite(residual[i])) {
                            norm = std::numeric_limits<double>::infinity();
                            break;
                        }
                        norm = std::max(norm, std::abs(residual[i]) / volume[i]);
                    }
                    if (std::isfinite(norm) &&
                        norm <= std::min(1.e-11, columns[a]->options.atol * 1.e-3))
                        done[a] = success[a] = true;
                    else
                        all = false;
                }
            if (all)
                break;
            std::fill(lo.begin(), lo.end(), 0);
            std::fill(up.begin(), up.end(), 0);
            std::fill(di.begin(), di.end(), 1);
            // Three-color differentiation of a nearest-neighbor stencil.
            // Each RHS sweep perturbs non-overlapping Jacobian columns.
            for (int color = 0; color < 3; ++color) {
                perturbed = out;
                for (int k = color; k < n; k += 3)
                    for (int a = 0; a < lanes; ++a)
                        if (!done[a]) {
                            int i = k * lanes + a;
                            // Resolve the narrow saturated storage branch rather than
                            // perturbing across it with a fixed moisture increment.
                            const double wet_distance =
                                std::abs(out[i] - cellAt(k, a).theta_s * volume[i]);
                            epsilon[i] =
                                1.e-8 * (k ? std::max(wet_distance, 1.e-4 * volume[i]) : volume[i]);
                            perturbed[i] += epsilon[i];
                        }
                rhs(perturbed, fp, nullptr, nullptr, &done, rates);
                for (int k = color; k < n; k += 3)
                    for (int a = 0; a < lanes; ++a)
                        if (!done[a]) {
                            int j = k * lanes + a;
                            for (int row = std::max(0, k - 1); row <= std::min(n - 1, k + 1);
                                 ++row) {
                                int i = row * lanes + a;
                                double value = -steps[a] * (fp[i] - f[i]) / epsilon[j];
                                if (row == k)
                                    di[i] += value;
                                else if (row < k)
                                    up[i] = value;
                                else
                                    lo[i] = value;
                            }
                        }
            }
            for (int k = 0; k < n; ++k)
                for (int a = 0; a < lanes; ++a) {
                    int i = k * lanes + a;
                    delta[i] = done[a] ? 0 : -residual[i];
                    if (k) {
                        double factor = lo[i] / di[i - lanes];
                        di[i] -= factor * up[i - lanes];
                        delta[i] -= factor * delta[i - lanes];
                    }
                }
            for (int k = n - 1; k >= 0; --k)
                for (int a = 0; a < lanes; ++a) {
                    int i = k * lanes + a;
                    if (k + 1 < n)
                        delta[i] -= up[i] * delta[i + lanes];
                    delta[i] /= di[i];
                }
            for (int a = 0; a < lanes; ++a)
                if (!done[a]) {
                    double damping = 1;
                    for (int k = 0; k < n; ++k) {
                        int i = k * lanes + a;
                        if (!std::isfinite(delta[i])) {
                            done[a] = true;
                            damping = 0;
                            break;
                        }
                        if (delta[i] < 0)
                            damping =
                                std::min(damping, 0.9 * (out[i] - lower_bound[i]) / -delta[i]);
                    }
                    if (damping <= 1.e-10) {
                        done[a] = true;
                        continue;
                    }
                    for (int k = 0; k < n; ++k)
                        out[k * lanes + a] += damping * delta[k * lanes + a];
                }
        }
    }
    /** @brief Integrate this packed group with independent adaptive lane clocks.
     * @param dt Finite positive interval seconds; columns/options/budgets already validated.
     * Uses full versus two half implicit stages for the error test; accepted half-step
     * endpoint fluxes produce chronological m3 transfer receipts. Opposing bottom budgets
     * remain separate and never recycle incoming awards within the interval. Updates
     * reports, including trial work on failure. A failed lane stops the group without
     * publishing column water; outer advance() rejects all groups. Successful completion
     * writes each borrowed trial column's water and balance. Rejects underflow or more
     * than 200,000 group attempts. Allocation failures propagate; no external state,
     * clock or reservation owner is committed by this internal trial operation. */
    void run(double dt) {
        std::vector<double> time(lanes, 0), step(lanes), full, half, two, f, faces1, faces2, et1,
            et2, h;
        std::vector<uint8_t> good(lanes), good1(lanes), good2(lanes), inactive(lanes);
        std::vector<int> limited1, limited2;
        BoundaryRates full_rates, half_rates, second_rates;
        for (int a = 0; a < lanes; ++a)
            step[a] = std::min(dt, columns[a]->options.max_step);
        const bool bounded = std::any_of(columns.begin(), columns.end(), [](const auto* c) {
            return std::isfinite(c->bottom_out_limit) || std::isfinite(c->bottom_in_limit);
        });
        auto rates = [&](BoundaryRates& b, const std::vector<double>& steps,
                         const std::vector<double>* first_faces = nullptr) {
            b.out.resize(lanes);
            b.in.resize(lanes);
            for (int a = 0; a < lanes; ++a) {
                const double first =
                    first_faces ? steps[a] * (*first_faces)[(n - 1) * lanes + a] : 0;
                b.out[a] = reservedRate(columns[a]->bottom_out_limit,
                                        reports[a]->bottom_outflow + std::max(0., first), steps[a]);
                b.in[a] = reservedRate(columns[a]->bottom_in_limit,
                                       reports[a]->bottom_inflow + std::max(0., -first), steps[a]);
            }
        };
        int attempts = 0;
        while (*std::min_element(time.begin(), time.end()) < dt) {
            if (++attempts > 200000) {
                for (auto* r : reports) {
                    r->ok = false;
                    r->error = "Richards adaptive-step limit exceeded";
                }
                return;
            }
            for (int a = 0; a < lanes; ++a)
                step[a] = std::min(step[a], std::max(0.0, dt - time[a]));
            if (bounded)
                rates(full_rates, step);
            implicit(y, step, full, good, bounded ? &full_rates : nullptr);
            h = step;
            for (auto& v : h)
                v *= 0.5;
            if (bounded)
                rates(half_rates, h);
            implicit(y, h, half, good1, bounded ? &half_rates : nullptr);
            for (int a = 0; a < lanes; ++a)
                inactive[a] = step[a] == 0;
            if (bounded) {
                rhs(half, f, &faces1, &et1, &inactive, &half_rates, &limited1);
                rates(second_rates, h, &faces1);
            }
            implicit(half, h, two, good2, bounded ? &second_rates : nullptr);
            if (!bounded)
                rhs(half, f, &faces1, &et1, &inactive);
            rhs(two, f, &faces2, &et2, &inactive, bounded ? &second_rates : nullptr,
                bounded ? &limited2 : nullptr);
            for (int a = 0; a < lanes; ++a) {
                if (step[a] == 0)
                    continue;
                auto& report = *reports[a];
                const auto& opt = columns[a]->options;
                double error = 0;
                for (int k = 0; k < n; ++k) {
                    int i = k * lanes + a;
                    double scale = opt.atol * volume[i] +
                                   opt.rtol * std::max(std::abs(two[i]), std::abs(y[i]));
                    error = std::max(error, std::abs(two[i] - full[i]) / scale);
                }
                if (good[a] && good1[a] && good2[a] && std::isfinite(error) && error <= 1) {
                    ++report.accepted;
                    report.min_step =
                        report.min_step > 0 ? std::min(report.min_step, step[a]) : step[a];
                    // Commit two accepted half steps and their endpoint flux
                    // integrals in chronological order, retaining directions.
                    for (int stage = 0; stage < 2; ++stage)
                        for (int k = 0; k < n; ++k) {
                            if (k == 0 && columns[a]->inflow > 0) {
                                const double received = columns[a]->inflow * h[a];
                                report.inflow += received;
                                report.transfers.push_back({-2, 0, received});
                            }
                            int i = k * lanes + a;
                            double v = h[a] * (stage ? faces2[i] : faces1[i]);
                            if (k + 1 < n) {
                                if (v > 0)
                                    report.transfers.push_back({k, k + 1, v});
                                else if (v < 0)
                                    report.transfers.push_back({k + 1, k, -v});
                            } else {
                                report.bottom_volume += v;
                                if (v > 0)
                                    report.bottom_outflow += v;
                                else if (v < 0)
                                    report.bottom_inflow -= v;
                                if (v > 0)
                                    report.transfers.push_back({k, -4, v});
                                else if (v < 0)
                                    report.transfers.push_back({-5, k, -v});
                            }
                            double e = h[a] * (stage ? et2[i] : et1[i]);
                            report.evaporation += e;
                            if (e > 0)
                                report.transfers.push_back({k, -3, e});
                        }
                    if (bounded)
                        for (const int limited : {limited1[a], limited2[a]}) {
                            report.bottom_out_limited += limited == 1;
                            report.bottom_in_limited += limited == -1;
                        }
                    for (int k = 0; k < n; ++k)
                        y[k * lanes + a] = two[k * lanes + a];
                    time[a] = std::min(dt, time[a] + step[a]);
                    step[a] = std::min(
                        opt.max_step,
                        step[a] * std::clamp(0.8 / std::sqrt(std::max(error, 1.e-8)), 0.5, 2.0));
                } else {
                    ++report.rejected;
                    step[a] *= std::isfinite(error) && error > 1
                                   ? std::clamp(0.8 / std::sqrt(error), 0.1, 0.5)
                                   : 0.5;
                    if (step[a] < 1.e-9 * std::max(1.0, dt)) {
                        report.ok = false;
                        report.error = "Richards step underflow / Newton failure";
                        return;
                    }
                }
            }
        }
        for (int a = 0; a < lanes; ++a) {
            auto& col = *columns[a];
            auto& report = *reports[a];
            double initial = std::accumulate(col.water.begin(), col.water.end(), 0.0), final = 0;
            for (int k = 0; k < n; ++k) {
                col.water[k] = y[k * lanes + a];
                final += col.water[k];
            }
            report.balance = final - initial + report.bottom_volume + report.evaporation;
            if (col.inflow > 0)
                report.balance -= report.inflow;
        }
    }
};
}
std::vector<Report> advance(std::vector<Column*>& columns, double dt) {
    perf::ProcessTimer process_timer(perf::ProcessPhase::richards);
    std::vector<Report> reports(columns.size());
    if (!std::isfinite(dt) || dt <= 0) {
        for (auto& r : reports) {
            r.ok = false;
            r.error = "Invalid Richards interval";
        }
        return reports;
    }
    // Work on copies: any failure is transactional across the complete batch.
    std::vector<Column> trial;
    trial.reserve(columns.size());
    std::set<const Column*> seen;
    for (const auto* c : columns) {
        if (!c || c->cells.size() < 2 || c->water.size() != c->cells.size() || !valid(c->options) ||
            !std::isfinite(c->bottom_K) || c->bottom_K < 0 || !std::isfinite(c->bottom_head) ||
            !std::isfinite(c->bottom_distance) || c->bottom_distance < 0 ||
            std::isnan(c->bottom_out_limit) || c->bottom_out_limit < 0 ||
            std::isnan(c->bottom_in_limit) || c->bottom_in_limit < 0 ||
            !std::isfinite(c->bottom_area) || c->bottom_area < 0 ||
            !std::isfinite(c->bottom_factor) || c->bottom_factor < 0 || c->bottom_factor > 1 ||
            !std::isfinite(c->inflow) || c->inflow < 0 || !seen.insert(c).second) {
            for (auto& r : reports) {
                r.ok = false;
                r.error = "Invalid Richards column";
            }
            return reports;
        }
        for (std::size_t k = 0; k < c->cells.size(); ++k) {
            const auto& cell = c->cells[k];
            if (!std::isfinite(cell.volume) || cell.volume <= 0 || !std::isfinite(cell.area) ||
                cell.area <= 0 || !std::isfinite(cell.z) || !std::isfinite(cell.dz) ||
                cell.dz <= 0 || !std::isfinite(cell.Ks) || cell.Ks < 0 ||
                !std::isfinite(cell.theta_s) || cell.theta_s <= 0 || cell.theta_s > 1 ||
                !std::isfinite(cell.wilting) || cell.wilting < 0 || cell.wilting >= cell.theta_s ||
                !std::isfinite(cell.potential_et) || cell.potential_et < 0 ||
                !std::isfinite(c->water[k]) ||
                (k ? (!valid(cell.material, cell.theta_s) ||
                      c->water[k] <= cell.material.theta_r * cell.volume)
                   : c->water[k] < 0)) {
                for (auto& r : reports) {
                    r.ok = false;
                    r.error = "Invalid Richards state/material";
                }
                return reports;
            }
        }
        trial.push_back(*c);
    }
    std::map<std::size_t, std::vector<std::size_t>> groups;
    for (std::size_t a = 0; a < trial.size(); ++a)
        groups[trial[a].cells.size()].push_back(a);
    // Only parallel jobs pack coefficients; serial batches retain borrowed cells.
    const auto runBatch = [&](std::vector<Column*> c, std::vector<Report*> r, bool packed) {
        if (packed && c.size() >= 8)
            Batch<true>(std::move(c), std::move(r)).run(dt);
        else
            Batch<false>(std::move(c), std::move(r)).run(dt);
    };
    bool parallel_complete = false;
#ifdef SWMM_USE_OPENMP
    // Small calls retain the original serial group walk. All jobs below work on
    // distinct trial columns/reports; only the ordered fold can publish a result.
    if (trial.size() >= 256 && threadinfo::ompMaxThreads() > 1) {
        struct Job {
            const std::vector<std::size_t>* ids;
            std::size_t first, last;
            std::exception_ptr exception;
        };
        std::vector<Job> jobs;
        for (const auto& [size, ids] : groups)
            for (std::size_t first = 0; first < ids.size(); first += 64)
                jobs.push_back({&ids, first, std::min(ids.size(), first + 64), {}});
        const int threads = static_cast<int>(std::min<std::size_t>(
            jobs.size(), static_cast<std::size_t>(threadinfo::ompMaxThreads())));
#pragma omp parallel for schedule(static) num_threads(threads)
        for (std::ptrdiff_t index = 0; index < static_cast<std::ptrdiff_t>(jobs.size()); ++index) {
            auto& job = jobs[index];
            try {
                std::vector<Column*> c;
                std::vector<Report*> r;
                for (auto j = job.first; j < job.last; ++j) {
                    const auto id = (*job.ids)[j];
                    c.push_back(&trial[id]);
                    r.push_back(&reports[id]);
                }
                runBatch(std::move(c), std::move(r), true);
            } catch (...) {
                job.exception = std::current_exception();
            }
        }
        for (std::size_t index = 0; index < jobs.size(); ++index) {
            const auto& job = jobs[index];
            if (job.exception)
                std::rethrow_exception(job.exception);
            bool failed = false;
            for (auto j = job.first; j < job.last; ++j)
                failed = failed || !reports[(*job.ids)[j]].ok;
            if (failed) {
                // Serial execution would not have visited these later jobs. Keep
                // its exact report prefix, including default counters/receipts.
                for (auto later = index + 1; later < jobs.size(); ++later)
                    for (auto j = jobs[later].first; j < jobs[later].last; ++j)
                        reports[(*jobs[later].ids)[j]] = Report{};
                for (auto& report : reports) {
                    report.ok = false;
                    if (report.error.empty())
                        report.error = "Richards batch rolled back";
                }
                return reports;
            }
        }
        parallel_complete = true;
    }
#endif
    if (!parallel_complete) {
        for (const auto& [size, ids] : groups)
            for (std::size_t first = 0; first < ids.size(); first += 64) {
                std::vector<Column*> c;
                std::vector<Report*> r;
                const auto last = std::min(ids.size(), first + 64);
                for (auto j = first; j < last; ++j) {
                    const auto id = ids[j];
                    c.push_back(&trial[id]);
                    r.push_back(&reports[id]);
                }
                runBatch(std::move(c), std::move(r), false);
                for (auto j = first; j < last; ++j)
                    if (!reports[ids[j]].ok) {
                        for (auto& report : reports) {
                            report.ok = false;
                            if (report.error.empty())
                                report.error = "Richards batch rolled back";
                        }
                        return reports;
                    }
            }
    }
    for (std::size_t a = 0; a < trial.size(); ++a)
        columns[a]->water = std::move(trial[a].water);
    return reports;
}
}
