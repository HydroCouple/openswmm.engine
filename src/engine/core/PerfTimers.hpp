#pragma once
// -----------------------------------------------------------------------------
// PerfTimers — lightweight, env-gated wall-clock accumulators used to attribute
// run time between the 2D surface solve, the 1D routing step, and the 2D-window
// (rainfall + coupling) overhead. Header-only (C++17 inline variables) so no
// CMake/link changes are needed; the ScopedTimer only touches a steady_clock at
// coarse call sites (per macro-window / per routing step), never a hot inner
// loop. The split is printed once from SWMMEngine::end() when OPENSWMM_PERF is
// set. Zero cost when the env var is unset except the clock reads themselves.
// -----------------------------------------------------------------------------
#include <chrono>

namespace openswmm::perf {

inline double sec_2d_window  = 0.0;  // full 2D advance window (rainfall+coupling+solve)
inline double sec_2d_advance = 0.0;  // pure 2D solve (solver_->advance)
inline double sec_1d_step    = 0.0;  // 1D routing (router_.step)

// Adds the elapsed wall time between construction and destruction to `acc`.
struct ScopedTimer {
    double* acc;
    std::chrono::steady_clock::time_point t0;
    explicit ScopedTimer(double& a) noexcept
        : acc(&a), t0(std::chrono::steady_clock::now()) {}
    ~ScopedTimer() noexcept {
        *acc += std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t0).count();
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
};

} // namespace openswmm::perf
