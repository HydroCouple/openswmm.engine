/**
 * @file RomQuantileGemm.hpp
 * @brief PR H4 — GEMM-accelerated ensemble reconstruction, shared between
 *        SpectralROM1D::computeQuantiles() and 2D SpectralROM::computeQuantiles().
 *
 * @details Both ROMs reconstruct the full ensemble depth/head field the same
 *          way: `ΔH[t,i] = Σ_j P[j,t]·a_ensemble[i,j]` over active modes j,
 *          for every (node t, member i) pair -- an O(N·M·k) hand-rolled
 *          triple loop that dominates ROM cost at large N (the roadmap's own
 *          perf table; CL-2d recorded it as the ~19% cap on that PR's own
 *          speedup). `P` (n_kept × n_nodes row-major) and `a_ensemble`
 *          (n_ensemble × n_kept row-major) are already laid out for a single
 *          `cblas_dgemm` call with no transposition copies:
 *
 *              recon (n_nodes × n_ensemble, node-major)
 *                  = P_active^T (n_nodes × k_active) · A_active^T (k_active × n_ensemble)
 *
 *          computed via `cblas_dgemm(..., TransA=Trans, TransB=Trans, ...)`
 *          directly on the (compacted) row-major buffers -- BLAS's transpose
 *          flags avoid a physical transpose, and the result lands exactly in
 *          the node-major layout `computeQuantiles()`'s existing sort/select
 *          loop already expects (recon[t*M+i]).
 *
 *          ACTIVE-MODE MASKING (load-bearing, not an optimization detail):
 *          the pre-GEMM loops in both ROMs skip inactive modes in
 *          RECONSTRUCTION, not merely in advance()'s ODE update
 *          (`if (!mode_active[j]) continue;`). A mode that was active in the
 *          past and has since deactivated can still hold a small nonzero
 *          "frozen" `a_ensemble` coefficient (advance() simply stops
 *          updating it once inactive; it does not zero it), so a naive
 *          full-k GEMM over every mode would silently reintroduce those
 *          frozen contributions and NOT match the pre-GEMM behavior. This
 *          function compacts `P`/`a_ensemble` down to active-mode rows/
 *          columns first -- same masking semantics, same result -- rather
 *          than zeroing anything in place (P is basis-owned and often const;
 *          a_ensemble is per-member and would need an O(M·k) sweep every
 *          call either way, so compaction is no more expensive and touches
 *          neither source buffer).
 *
 *          Apple's Accelerate framework provides a complete CBLAS
 *          implementation as part of the base OS (`OPENSWMM_HAVE_CBLAS`,
 *          set by CMake when found -- zero new dependency). Everywhere else
 *          this falls back to a portable compacted triple loop: correct
 *          everywhere, BLAS-accelerated only where Accelerate is linked.
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_ROM_QUANTILE_GEMM_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_ROM_QUANTILE_GEMM_HPP

#include <algorithm>
#include <cstddef>
#include <vector>

#if defined(OPENSWMM_HAVE_CBLAS)
#include <Accelerate/Accelerate.h>
#endif

namespace openswmm::uncertainty {

/**
 * @brief Reconstruct the full ensemble field ΔH = A·P (active modes only),
 *        written node-major to match the existing per-node quantile loop.
 *
 * @param P            Full mode matrix, n_kept × n_nodes row-major
 *                      (`P[j*n_nodes+t]`) -- GraphEigenBasis::P / MeshEigenBasis::P.
 * @param a_ensemble   Full ensemble coefficient matrix, n_ensemble × n_kept
 *                      row-major (`a_ensemble[i*n_kept+j]`).
 * @param mode_active  Length n_kept; only active modes contribute (matches
 *                      the pre-GEMM loops' masking exactly -- see file doc).
 * @param n_nodes      Number of active nodes / triangles (N).
 * @param n_kept       Total retained modes (k).
 * @param n_ensemble   Ensemble size (M).
 * @param recon        Output, length n_nodes*n_ensemble, node-major
 *                      (`recon[t*n_ensemble+i]`). OVERWRITTEN, not accumulated.
 * @param P_active_buf Scratch, resized internally to k_active*n_nodes.
 *                      Caller-owned so repeated calls reuse the allocation.
 * @param A_active_buf Scratch, resized internally to n_ensemble*k_active.
 *                      Caller-owned so repeated calls reuse the allocation.
 */
inline void reconstructEnsembleGemm(const double* P, const double* a_ensemble,
                                     const std::vector<bool>& mode_active,
                                     int n_nodes, int n_kept, int n_ensemble,
                                     double* recon,
                                     std::vector<double>& P_active_buf,
                                     std::vector<double>& A_active_buf) noexcept {
    const auto nn = static_cast<std::size_t>(n_nodes);
    const auto nk = static_cast<std::size_t>(n_kept);
    const auto M  = static_cast<std::size_t>(n_ensemble);

    std::vector<int> active_modes;
    active_modes.reserve(nk);
    for (std::size_t j = 0; j < nk; ++j)
        if (mode_active[j]) active_modes.push_back(static_cast<int>(j));
    const std::size_t k_active = active_modes.size();

    if (k_active == 0 || nn == 0 || M == 0) {
        std::fill(recon, recon + nn * M, 0.0);
        return;
    }

    P_active_buf.resize(k_active * nn);
    for (std::size_t r = 0; r < k_active; ++r) {
        const double* src = &P[static_cast<std::size_t>(active_modes[r]) * nn];
        std::copy(src, src + nn, &P_active_buf[r * nn]);
    }
    A_active_buf.resize(M * k_active);
    for (std::size_t i = 0; i < M; ++i) {
        const double* row = &a_ensemble[i * nk];
        double* dst = &A_active_buf[i * k_active];
        for (std::size_t r = 0; r < k_active; ++r)
            dst[r] = row[static_cast<std::size_t>(active_modes[r])];
    }

#if defined(OPENSWMM_HAVE_CBLAS)
    // The legacy (non-ILP64) CBLAS interface is deprecated but still fully
    // supported on every macOS version this project targets; opting into
    // ACCELERATE_NEW_LAPACK project-wide is a separate, unrelated decision
    // (it also touches LAPACK integer width, which nothing here uses), so
    // this silences just the one known-fine deprecation instead.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    cblas_dgemm(CblasRowMajor, CblasTrans, CblasTrans,
                static_cast<int>(nn), static_cast<int>(M), static_cast<int>(k_active),
                1.0, P_active_buf.data(), static_cast<int>(nn),
                A_active_buf.data(), static_cast<int>(k_active),
                0.0, recon, static_cast<int>(M));
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#else
    // Portable fallback: same access pattern as the pre-GEMM hand-rolled
    // loop (mode-outer, node-middle, member-inner), just over the compacted
    // active-only matrices instead of a masked full-k sweep.
    std::fill(recon, recon + nn * M, 0.0);
    for (std::size_t r = 0; r < k_active; ++r) {
        const double* Pr = &P_active_buf[r * nn];
        for (std::size_t t = 0; t < nn; ++t) {
            const double pjt = Pr[t];
            double* row = &recon[t * M];
            for (std::size_t i = 0; i < M; ++i)
                row[i] += pjt * A_active_buf[i * k_active + r];
        }
    }
#endif
}

}  // namespace openswmm::uncertainty

#endif  // OPENSWMM_ENGINE_UNCERTAINTY_ROM_QUANTILE_GEMM_HPP
