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
#include <cstdio>
#include <cstdlib>

namespace openswmm::perf {

inline double sec_2d_window  = 0.0;  // full 2D advance window (rainfall+coupling+solve)
inline double sec_2d_advance = 0.0;  // pure 2D solve (solver_->advance)
inline double sec_1d_step    = 0.0;  // 1D routing (router_.step)

// ---------------------------------------------------------------------------
// Load-phase accumulators — the open() / initialize() / start() window, which
// is what the user sees between clicking Run and the first routing step. Same
// env gate and same ScopedTimer as above; reset by open() so a process that
// runs several models reports each one separately.
// ---------------------------------------------------------------------------

inline double sec_open_prescan2d = 0.0;  // twoD::prescan2DUnitsHeader (whole-file pass)
inline double sec_open_read      = 0.0;  // input_plugin->read (tokenize + handlers)
// open.read split: the line scan (getline, trim, section_lines assembly) is
// everything in read() that is not a section handler. Dispatch is measured and
// scan is derived as the remainder, so the two always sum to open.read.
inline double sec_read_dispatch  = 0.0;  // SectionRegistry handler execution
inline double sec_open_resolve   = 0.0;  // input::resolve_cross_references
inline double sec_open_validate  = 0.0;  // validate_project

// resolve_cross_references sub-phases (sum <= sec_open_resolve; the remainder
// is everything not individually bracketed).
inline double sec_res_extfiles   = 0.0;  // FILE-backed timeseries/curve loads
inline double sec_res_tables     = 0.0;  // gage/inflow/table name bindings
inline double sec_res_transects  = 0.0;  // transect + street/inlet resolution
inline double sec_res_xsect      = 0.0;  // per-link cross-section parameter loop
inline double sec_res_shrink     = 0.0;  // shrink_all_to_fit

inline double sec_init_state      = 0.0; // initialize() body before init_modules()
inline double sec_init_hydraulics = 0.0; // initHydraulics (router_.init, FV mesh build)
inline double sec_init_hydrology  = 0.0; // initHydrology
inline double sec_init_quality    = 0.0; // initQuality
inline double sec_init_geometry   = 0.0; // initGeometry

inline double sec_start_iface   = 0.0;   // [FILES] interface-file open block
inline double sec_start_plugins = 0.0;   // plugins_.prepare_all (report preamble + .out header)

/** @brief Manual timing pair, for phases that do not fit a lexical scope. */
inline std::chrono::steady_clock::time_point now() noexcept {
    return std::chrono::steady_clock::now();
}
inline double since(std::chrono::steady_clock::time_point t0) noexcept {
    return std::chrono::duration<double>(now() - t0).count();
}

/** @brief True when OPENSWMM_PERF is set. Cached — getenv is not free. */
inline bool enabled() noexcept {
    static const bool on = (std::getenv("OPENSWMM_PERF") != nullptr);
    return on;
}

/** @brief Zeroes the load-phase accumulators. Called from open(). */
inline void reset_load() noexcept {
    sec_open_prescan2d = sec_open_read = sec_open_resolve = sec_open_validate = 0.0;
    sec_read_dispatch = 0.0;
    sec_res_extfiles = sec_res_tables = sec_res_transects = 0.0;
    sec_res_xsect = sec_res_shrink = 0.0;
    sec_init_state = sec_init_hydraulics = sec_init_hydrology = 0.0;
    sec_init_quality = sec_init_geometry = 0.0;
    sec_start_iface = sec_start_plugins = 0.0;
}

/**
 * @brief One machine-scrapeable line per phase on stderr.
 * @details Emitted from close() so all three benchmark cuts (open only,
 *          open+initialize, full) report. Format is `[PERF-LOAD] key=value`
 *          pairs in seconds; tests/benchmarks/scripts/ parses it.
 */
inline void dump_load() noexcept {
    const double open_total = sec_open_prescan2d + sec_open_read
                            + sec_open_resolve + sec_open_validate;
    const double init_total = sec_init_state + sec_init_hydraulics
                            + sec_init_hydrology + sec_init_quality
                            + sec_init_geometry;
    const double start_total = sec_start_iface + sec_start_plugins;
    std::fprintf(stderr,
        "[PERF-LOAD] open=%.4f open.prescan2d=%.4f open.read=%.4f "
        "read.scan=%.4f read.dispatch=%.4f "
        "open.resolve=%.4f open.validate=%.4f "
        "res.extfiles=%.4f res.tables=%.4f res.transects=%.4f res.xsect=%.4f "
        "res.shrink=%.4f "
        "init=%.4f init.state=%.4f init.hydraulics=%.4f init.hydrology=%.4f "
        "init.quality=%.4f init.geometry=%.4f "
        "start=%.4f start.iface=%.4f start.plugins=%.4f\n",
        open_total, sec_open_prescan2d, sec_open_read,
        sec_open_read - sec_read_dispatch, sec_read_dispatch,
        sec_open_resolve, sec_open_validate,
        sec_res_extfiles, sec_res_tables, sec_res_transects, sec_res_xsect,
        sec_res_shrink,
        init_total, sec_init_state, sec_init_hydraulics, sec_init_hydrology,
        sec_init_quality, sec_init_geometry,
        start_total, sec_start_iface, sec_start_plugins);
}

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
