// SPDX-License-Identifier: Apache-2.0
#include "RichardsColumn.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

namespace openswmm::richards {
bool valid(const Material& p, double ts) {
    return std::isfinite(ts) && ts > 0 && ts <= 1 && std::isfinite(p.theta_r) && p.theta_r >= 0 && p.theta_r < ts &&
        std::isfinite(p.alpha) && p.alpha > 0 && std::isfinite(p.n) && p.n > 1 &&
        std::isfinite(p.l) && p.l >= 0 && std::isfinite(p.specific_storage) && p.specific_storage > 0;
}
bool valid(const Options& o) {
    return o.cells_per_layer >= 1 && o.cells_per_layer <= 256 &&
        std::isfinite(o.atol) && o.atol > 0 && std::isfinite(o.rtol) && o.rtol > 0 &&
        std::isfinite(o.max_step) && o.max_step > 0;
}
double theta(const Material& p, double ts, double h) {
    if (h >= 0) return ts;
    const double m = 1 - 1 / p.n;
    return p.theta_r + (ts - p.theta_r) * std::pow(1 + std::pow(-p.alpha * h, p.n), -m);
}
double storage(const Material& p, double ts, double h) {
    return theta(p, ts, h) + p.specific_storage * std::max(0.0, h);
}
double pressure(const Material& p, double ts, double w) {
    if (w >= ts) return (w - ts) / p.specific_storage;
    if (w <= p.theta_r) return -std::numeric_limits<double>::infinity();
    const double se = (w - p.theta_r) / (ts - p.theta_r), m = 1 - 1 / p.n;
    return -std::pow(std::expm1(-std::log(se) / m), 1 / p.n) / p.alpha;
}
double conductivity(const Material& p, double Ks, double h) {
    if (h >= 0) return Ks;
    const double m = 1 - 1 / p.n;
    const double se = std::pow(1 + std::pow(-p.alpha * h, p.n), -m);
    const double term = -std::expm1(m * std::log1p(-std::pow(se, 1 / m)));
    return Ks * std::pow(se, p.l) * term * term;
}
namespace {
struct Batch {
    int n, lanes;
    std::vector<Column*> columns;
    std::vector<double> y, volume, lower_bound;
    std::vector<Report*> reports;
    Batch(std::vector<Column*> c, std::vector<Report*> r) : n(c.front()->cells.size()), lanes(c.size()), columns(std::move(c)), reports(std::move(r)) {
        y.resize(n * lanes); volume.resize(y.size()); lower_bound.resize(y.size());
        for (int k = 0; k < n; ++k) for (int a = 0; a < lanes; ++a) {
            const auto& cell = columns[a]->cells[k]; const int i = k * lanes + a;
            y[i] = columns[a]->water[k]; volume[i] = cell.volume;
            lower_bound[i] = k ? cell.material.theta_r * cell.volume : 0.0;
        }
    }
    // Flux positive downward. Surface wet/dry transition is continuous over
    // 1 micrometre of ponding; no water is discarded at that transition.
    void rhs(const std::vector<double>& w, std::vector<double>& f,
             std::vector<double>* faces = nullptr, std::vector<double>* et = nullptr,
             const std::vector<bool>* inactive = nullptr) {
        f.assign(w.size(), 0.0);
        std::vector<double> head(w.size()), K(w.size());
        if (faces) faces->assign(n * lanes, 0.0);
        if (et) et->assign(w.size(), 0.0);
        for (int a = 0; a < lanes; ++a) if (!inactive || !(*inactive)[a]) ++reports[a]->rhs;
        for (int k = 0; k < n; ++k) for (int a = 0; a < lanes; ++a) {
            if (inactive && (*inactive)[a]) continue;
            const int i = k * lanes + a; const auto& c = columns[a]->cells[k];
            head[i] = c.z + (k ? pressure(c.material, c.theta_s, w[i] / c.volume) : std::max(0.0, w[i]) / (c.area * c.theta_s));
            K[i] = k ? conductivity(c.material, c.Ks, head[i] - c.z) : 0.0;
            double stress = k ? std::clamp((w[i] / c.volume - c.wilting) /
                std::max(c.theta_s - c.wilting, 1.e-12), 0.0, 1.0) :
                std::clamp(w[i] / (c.area * c.theta_s * 1.e-6), 0.0, 1.0);
            const double e = c.potential_et * stress;
            f[i] -= e; if (et) (*et)[i] = e;
        }
        for (int k = 0; k < n - 1; ++k) for (int a = 0; a < lanes; ++a) {
            if (inactive && (*inactive)[a]) continue;
            const int i = k * lanes + a, j = i + lanes;
            const auto& c = columns[a]->cells[k]; const auto& d = columns[a]->cells[k + 1];
            const double area = std::min(c.area, d.area);
            double conductance = 0.0;
            // Arithmetic conductivity within a material follows openRE
            // (Ireson et al. 2023, Eq. 11/14). Using the dry cell's K alone
            // at a ponded boundary can numerically lock the wetting front.
            if (k == 0) conductance = area * (d.Ks + K[j]) / d.dz;
            else if (c.material.alpha == d.material.alpha && c.material.n == d.material.n &&
                     c.material.theta_r == d.material.theta_r && c.material.l == d.material.l && c.Ks == d.Ks)
                conductance = area * (c.dz * K[j] + d.dz * K[i]) / (c.dz + d.dz) / (0.5 * (c.dz + d.dz));
            else if (K[i] > 0 && K[j] > 0)
                conductance = area / (0.5 * c.dz / K[i] + 0.5 * d.dz / K[j]);
            double q = conductance * (head[i] - head[j]);
            if (k == 0 && q > 0) q *= std::clamp(w[i] / (c.area * c.theta_s * 1.e-6), 0.0, 1.0);
            f[i] -= q; f[j] += q;
            if (faces) (*faces)[i] = q;
        }
        for (int a = 0; a < lanes; ++a) {
            if (inactive && (*inactive)[a]) continue;
            const int i = (n - 1) * lanes + a; const auto& col = *columns[a]; const auto& c = col.cells.back();
            double q = 0.0;
            if (col.bottom == Bottom::FreeDrainage) q = std::min(K[i], col.bottom_K) * c.area;
            else if (col.bottom == Bottom::Head && K[i] > 0 && col.bottom_K > 0) {
                const double resistance = 0.5 * c.dz / K[i] + col.bottom_distance / col.bottom_K;
                q = c.area * (head[i] - col.bottom_head) / resistance;
            }
            f[i] -= q; if (faces) (*faces)[i] = q;
        }
    }
    std::vector<bool> implicit(const std::vector<double>& start, const std::vector<double>& steps, std::vector<double>& out) {
        out = start;
        std::vector<bool> done(lanes, false), success(lanes, false);
        std::vector<double> f, fp, residual(y.size()), lo(y.size()), di(y.size()), up(y.size()), delta(y.size()), perturbed, epsilon(y.size());
        for (int a = 0; a < lanes; ++a) if (steps[a] == 0) done[a] = success[a] = true;
        for (int it = 0; it < 35; ++it) {
            rhs(out, f, nullptr, nullptr, &done);
            bool all = true;
            for (int a = 0; a < lanes; ++a) if (!done[a]) {
                ++reports[a]->newton;
                double norm = 0;
                for (int k = 0; k < n; ++k) {
                    int i = k * lanes + a;
                    residual[i] = out[i] - start[i] - steps[a] * f[i];
                    if (!std::isfinite(residual[i])) { norm = std::numeric_limits<double>::infinity(); break; }
                    norm = std::max(norm, std::abs(residual[i]) / volume[i]);
                }
                if (std::isfinite(norm) && norm <= std::min(1.e-11, columns[a]->options.atol * 1.e-3)) done[a] = success[a] = true;
                else all = false;
            }
            if (all) break;
            std::fill(lo.begin(), lo.end(), 0); std::fill(up.begin(), up.end(), 0); std::fill(di.begin(), di.end(), 1);
            // Three-color differentiation of a nearest-neighbor stencil.
            // Each RHS sweep perturbs non-overlapping Jacobian columns.
            for (int color = 0; color < 3; ++color) {
                perturbed = out;
                for (int k = color; k < n; k += 3) for (int a = 0; a < lanes; ++a) if (!done[a]) {
                    int i = k * lanes + a;
                    // Resolve the narrow saturated storage branch rather than
                    // perturbing across it with a fixed moisture increment.
                    const double wet_distance = std::abs(out[i] - columns[a]->cells[k].theta_s * volume[i]);
                    epsilon[i] = 1.e-8 * (k ? std::max(wet_distance, 1.e-4 * volume[i]) : volume[i]);
                    perturbed[i] += epsilon[i];
                }
                rhs(perturbed, fp, nullptr, nullptr, &done);
                for (int k = color; k < n; k += 3) for (int a = 0; a < lanes; ++a) if (!done[a]) {
                    int j = k * lanes + a;
                    for (int row = std::max(0, k - 1); row <= std::min(n - 1, k + 1); ++row) {
                        int i = row * lanes + a;
                        double value = -steps[a] * (fp[i] - f[i]) / epsilon[j];
                        if (row == k) di[i] += value;
                        else if (row < k) up[i] = value;
                        else lo[i] = value;
                    }
                }
            }
            for (int k = 0; k < n; ++k) for (int a = 0; a < lanes; ++a) {
                int i = k * lanes + a;
                delta[i] = done[a] ? 0 : -residual[i];
                if (k) { double factor = lo[i] / di[i - lanes]; di[i] -= factor * up[i - lanes]; delta[i] -= factor * delta[i - lanes]; }
            }
            for (int k = n - 1; k >= 0; --k) for (int a = 0; a < lanes; ++a) {
                int i = k * lanes + a;
                if (k + 1 < n) delta[i] -= up[i] * delta[i + lanes];
                delta[i] /= di[i];
            }
            for (int a = 0; a < lanes; ++a) if (!done[a]) {
                double damping = 1;
                for (int k = 0; k < n; ++k) {
                    int i = k * lanes + a;
                    if (!std::isfinite(delta[i])) { done[a] = true; damping = 0; break; }
                    if (delta[i] < 0) damping = std::min(damping, 0.9 * (out[i] - lower_bound[i]) / -delta[i]);
                }
                if (damping <= 1.e-10) { done[a] = true; continue; }
                for (int k = 0; k < n; ++k) out[k * lanes + a] += damping * delta[k * lanes + a];
            }
        }
        return success;
    }
    void run(double dt) {
        std::vector<double> time(lanes, 0), step(lanes), full, half, two, f, faces1, faces2, et1, et2;
        for (int a = 0; a < lanes; ++a) step[a] = std::min(dt, columns[a]->options.max_step);
        int attempts = 0;
        while (*std::min_element(time.begin(), time.end()) < dt) {
            if (++attempts > 200000) { for (auto* r : reports) { r->ok = false; r->error = "Richards adaptive-step limit exceeded"; } return; }
            for (int a = 0; a < lanes; ++a) step[a] = std::min(step[a], std::max(0.0, dt - time[a]));
            auto good = implicit(y, step, full);
            auto h = step; for (auto& v : h) v *= 0.5;
            auto good1 = implicit(y, h, half), good2 = implicit(half, h, two);
            std::vector<bool> inactive(lanes); for (int a = 0; a < lanes; ++a) inactive[a] = step[a] == 0;
            rhs(half, f, &faces1, &et1, &inactive); rhs(two, f, &faces2, &et2, &inactive);
            for (int a = 0; a < lanes; ++a) {
                if (step[a] == 0) continue;
                auto& report = *reports[a]; const auto& opt = columns[a]->options;
                double error = 0;
                for (int k = 0; k < n; ++k) {
                    int i = k * lanes + a;
                    double scale = opt.atol * volume[i] + opt.rtol * std::max(std::abs(two[i]), std::abs(y[i]));
                    error = std::max(error, std::abs(two[i] - full[i]) / scale);
                }
                if (good[a] && good1[a] && good2[a] && std::isfinite(error) && error <= 1) {
                    ++report.accepted;
                    report.min_step = report.min_step > 0 ? std::min(report.min_step, step[a]) : step[a];
                    // Commit two accepted half steps and their endpoint flux
                    // integrals in chronological order, retaining directions.
                    for (int stage = 0; stage < 2; ++stage) for (int k = 0; k < n; ++k) {
                        int i = k * lanes + a;
                        double v = h[a] * (stage ? faces2[i] : faces1[i]);
                        if (k + 1 < n) {
                            if (v > 0) report.transfers.push_back({k, k + 1, v});
                            else if (v < 0) report.transfers.push_back({k + 1, k, -v});
                        } else {
                            report.bottom_volume += v;
                            if (v > 0) report.transfers.push_back({k, -4, v});
                            else if (v < 0) report.transfers.push_back({-5, k, -v});
                        }
                        double e = h[a] * (stage ? et2[i] : et1[i]);
                        report.evaporation += e;
                        if (e > 0) report.transfers.push_back({k, -3, e});
                    }
                    for (int k = 0; k < n; ++k) y[k * lanes + a] = two[k * lanes + a];
                    time[a] = std::min(dt, time[a] + step[a]);
                    step[a] = std::min(opt.max_step, step[a] * std::clamp(0.8 / std::sqrt(std::max(error, 1.e-8)), 0.5, 2.0));
                } else {
                    ++report.rejected;
                    step[a] *= std::isfinite(error) && error > 1 ? std::clamp(0.8 / std::sqrt(error), 0.1, 0.5) : 0.5;
                    if (step[a] < 1.e-9 * std::max(1.0, dt)) { report.ok = false; report.error = "Richards step underflow / Newton failure"; return; }
                }
            }
        }
        for (int a = 0; a < lanes; ++a) {
            auto& col = *columns[a]; auto& report = *reports[a];
            double initial = std::accumulate(col.water.begin(), col.water.end(), 0.0), final = 0;
            for (int k = 0; k < n; ++k) { col.water[k] = y[k * lanes + a]; final += col.water[k]; }
            report.balance = final - initial + report.bottom_volume + report.evaporation;
        }
    }
};
}
std::vector<Report> advance(std::vector<Column*>& columns, double dt) {
    std::vector<Report> reports(columns.size());
    if (!std::isfinite(dt) || dt <= 0) { for (auto& r : reports) { r.ok = false; r.error = "Invalid Richards interval"; } return reports; }
    // Work on copies: any failure is transactional across the complete batch.
    std::vector<Column> trial; trial.reserve(columns.size());
    for (const auto* c : columns) {
        if (!c || c->cells.size() < 2 || c->water.size() != c->cells.size() || !valid(c->options) ||
            !std::isfinite(c->bottom_K) || c->bottom_K < 0 || !std::isfinite(c->bottom_head) ||
            !std::isfinite(c->bottom_distance) || c->bottom_distance < 0) {
            for (auto& r : reports) { r.ok = false; r.error = "Invalid Richards column"; } return reports;
        }
        for (std::size_t k = 0; k < c->cells.size(); ++k) {
            const auto& cell = c->cells[k];
            if (!std::isfinite(cell.volume) || cell.volume <= 0 || !std::isfinite(cell.area) || cell.area <= 0 ||
                !std::isfinite(cell.z) || !std::isfinite(cell.dz) || cell.dz <= 0 || !std::isfinite(cell.Ks) || cell.Ks < 0 ||
                !std::isfinite(cell.theta_s) || cell.theta_s <= 0 || cell.theta_s > 1 ||
                !std::isfinite(cell.wilting) || cell.wilting < 0 || cell.wilting >= cell.theta_s ||
                !std::isfinite(cell.potential_et) || cell.potential_et < 0 || !std::isfinite(c->water[k]) ||
                (k ? (!valid(cell.material, cell.theta_s) || c->water[k] <= cell.material.theta_r * cell.volume) : c->water[k] < 0)) {
                for (auto& r : reports) { r.ok = false; r.error = "Invalid Richards state/material"; } return reports;
            }
        }
        trial.push_back(*c);
    }
    std::map<std::size_t, std::vector<std::size_t>> groups;
    for (std::size_t a = 0; a < trial.size(); ++a) groups[trial[a].cells.size()].push_back(a);
    for (const auto& [size, ids] : groups) for (std::size_t first = 0; first < ids.size(); first += 64) {
        std::vector<Column*> c; std::vector<Report*> r;
        const auto last = std::min(ids.size(), first + 64);
        for (auto j = first; j < last; ++j) { const auto id = ids[j]; c.push_back(&trial[id]); r.push_back(&reports[id]); }
        Batch(std::move(c), std::move(r)).run(dt);
        for (auto j = first; j < last; ++j) if (!reports[ids[j]].ok) {
            for (auto& report : reports) { report.ok = false; if (report.error.empty()) report.error = "Richards batch rolled back"; }
            return reports;
        }
    }
    for (std::size_t a = 0; a < trial.size(); ++a) columns[a]->water = std::move(trial[a].water);
    return reports;
}
}
