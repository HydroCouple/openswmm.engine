/**
 * @file CorrelatedFieldGenerator.cpp
 * @see CorrelatedFieldGenerator.hpp
 */

#ifdef OPENSWMM_HAS_2D

#include "CorrelatedFieldGenerator.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace openswmm::twoD {

// ---------------------------------------------------------------------------
// PRNG helpers
// ---------------------------------------------------------------------------

uint64_t CorrelatedFieldGenerator::splitmix64(uint64_t& state) noexcept {
    uint64_t z = (state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

double CorrelatedFieldGenerator::boxMuller(uint64_t& state) noexcept {
    // Draw two U(0,1) values and apply Box-Muller.
    constexpr double kInv = 1.0 / static_cast<double>(UINT64_MAX);
    double u1, u2;
    do { u1 = static_cast<double>(splitmix64(state)) * kInv; } while (u1 == 0.0);
    u2 = static_cast<double>(splitmix64(state)) * kInv;
    constexpr double kTwoPi = 6.283185307179586;
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(kTwoPi * u2);
}

// ---------------------------------------------------------------------------
// Neighbourhood construction (grid-indexed, O(n · avg_neighbors))
// ---------------------------------------------------------------------------

std::vector<std::vector<int>>
CorrelatedFieldGenerator::buildNeighbourhood(const double* cx, const double* cy,
                                              int n_tri, double radius) {
    if (n_tri <= 0 || radius <= 0.0) {
        // Empty neighbourhood: each cell is only its own neighbour.
        std::vector<std::vector<int>> nbhd(static_cast<std::size_t>(n_tri));
        for (int t = 0; t < n_tri; ++t)
            nbhd[static_cast<std::size_t>(t)] = {t};
        return nbhd;
    }

    // Compute bounding box.
    double xmin = cx[0], xmax = cx[0], ymin = cy[0], ymax = cy[0];
    for (int t = 1; t < n_tri; ++t) {
        xmin = std::min(xmin, cx[t]);  xmax = std::max(xmax, cx[t]);
        ymin = std::min(ymin, cy[t]);  ymax = std::max(ymax, cy[t]);
    }

    // Grid cell size = radius (so adjacent 3×3 block covers 3*radius search).
    const double inv_cell = 1.0 / radius;
    const int nx = std::max(1, static_cast<int>(std::ceil((xmax - xmin) * inv_cell)) + 1);
    const int ny = std::max(1, static_cast<int>(std::ceil((ymax - ymin) * inv_cell)) + 1);

    // Assign each triangle to a grid cell.
    std::vector<int> grid_id(static_cast<std::size_t>(n_tri));
    std::vector<std::vector<int>> grid(static_cast<std::size_t>(nx * ny));
    for (int t = 0; t < n_tri; ++t) {
        const int gx = static_cast<int>((cx[t] - xmin) * inv_cell);
        const int gy = static_cast<int>((cy[t] - ymin) * inv_cell);
        const int gid = gx * ny + gy;
        grid_id[static_cast<std::size_t>(t)] = gid;
        grid[static_cast<std::size_t>(gid)].push_back(t);
    }

    const double r2 = radius * radius;

    std::vector<std::vector<int>> nbhd(static_cast<std::size_t>(n_tri));
    for (int t = 0; t < n_tri; ++t) {
        const double xt = cx[t], yt = cy[t];
        const int gx = static_cast<int>((xt - xmin) * inv_cell);
        const int gy = static_cast<int>((yt - ymin) * inv_cell);

        auto& out = nbhd[static_cast<std::size_t>(t)];
        // Search 3×3 block of grid cells.
        for (int dgx = -1; dgx <= 1; ++dgx) {
            const int ix = gx + dgx;
            if (ix < 0 || ix >= nx) continue;
            for (int dgy = -1; dgy <= 1; ++dgy) {
                const int iy = gy + dgy;
                if (iy < 0 || iy >= ny) continue;
                for (int s : grid[static_cast<std::size_t>(ix * ny + iy)]) {
                    const double dx = cx[s] - xt, dy = cy[s] - yt;
                    if (dx * dx + dy * dy <= r2)
                        out.push_back(s);
                }
            }
        }
        // Always include self (weight = exp(0) = 1).
        if (out.empty()) out.push_back(t);
    }
    return nbhd;
}

// ---------------------------------------------------------------------------
// Main generation
// ---------------------------------------------------------------------------

void CorrelatedFieldGenerator::generate(const double* cx, const double* cy,
                                         int n_tri,
                                         const std::vector<double>& scalar_mult,
                                         double pert, double corr_len,
                                         uint64_t seed,
                                         SpatialUncertaintyField& out) {
    const int M = static_cast<int>(scalar_mult.size());
    if (M <= 0 || n_tri <= 0)
        throw std::invalid_argument("CorrelatedFieldGenerator: n_members and n_tri must be > 0");

    out.allocate(M, n_tri);

    // Scalar mode: W[i][t] = scalar_mult[i] for all t.
    if (corr_len <= 0.0 || pert <= 0.0) {
        out.fromScalar(scalar_mult, n_tri);
        return;
    }

    // Build grid-indexed neighbourhood within 3 correlation lengths.
    const double search_radius = 3.0 * corr_len;
    auto nbhd = buildNeighbourhood(cx, cy, n_tri, search_radius);

    // Precompute centroid coordinates as flat arrays for PRNG loop.
    // (cx, cy already flat — use directly.)

    std::vector<double> z_raw(static_cast<std::size_t>(n_tri));
    std::vector<double> z_smooth(static_cast<std::size_t>(n_tri));

    const double inv_ell = 1.0 / corr_len;
    constexpr double kEps = 1.0e-6;  // clamp floor/ceiling guard

    uint64_t rng_state = seed ^ UINT64_C(0xcafebabe12345678);

    for (int i = 0; i < M; ++i) {
        // Step 1: draw n_tri i.i.d. N(0,1).
        for (int t = 0; t < n_tri; ++t)
            z_raw[static_cast<std::size_t>(t)] = boxMuller(rng_state);

        // Step 2: smooth with exponential kernel.
        for (int t = 0; t < n_tri; ++t) {
            const auto& nb = nbhd[static_cast<std::size_t>(t)];
            const double xt = cx[t], yt = cy[t];
            double num = 0.0, denom = 0.0;
            for (int s : nb) {
                const double dx = cx[s] - xt, dy = cy[s] - yt;
                const double d  = std::sqrt(dx * dx + dy * dy);
                const double w  = std::exp(-d * inv_ell);
                num   += w * z_raw[static_cast<std::size_t>(s)];
                denom += w;
            }
            z_smooth[static_cast<std::size_t>(t)] = num / denom;
        }

        // Step 3: centre the smooth field (subtract mean across cells).
        // Intentionally do NOT standardise: leaving the raw amplitude intact
        // means that as corr_len grows (more neighbours → stronger averaging),
        // the spatial variance naturally collapses toward zero — i.e. very large
        // corr_len produces a nearly uniform field, which is the expected physical
        // behaviour.  For typical corr_len ~ mesh spacing, each cell averages only
        // a few neighbours so the amplitude is close to N(0,1).
        double mean_z = 0.0;
        for (int t = 0; t < n_tri; ++t)
            mean_z += z_smooth[static_cast<std::size_t>(t)];
        mean_z /= n_tri;

        // Step 4: map to multiplier W[i][t] = scalar_mult[i] + pert * z_centred[t].
        // The global LHS member level (scalar_mult[i]) centres the spatial field;
        // pert scales the local spatial deviation from that level.
        const double base = scalar_mult[static_cast<std::size_t>(i)];
        double* row = out.values.data() + static_cast<std::size_t>(i) * n_tri;
        for (int t = 0; t < n_tri; ++t) {
            const double z_c = z_smooth[static_cast<std::size_t>(t)] - mean_z;
            const double val = base + pert * z_c;
            row[t] = std::max(kEps, std::min(2.0 - kEps, val));
        }
    }
}

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
