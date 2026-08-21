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

// ---------------------------------------------------------------------------
// FV 1D solver phase breakdown. `sec_1d_step` brackets the whole router step,
// which is enough to say the FV solver is slow and nothing about WHY. These
// split it by phase so a plan can be ranked against a profile instead of a
// recollection.
//
// Granularity is per-substep-phase, never per-cell or per-face: at hundreds of
// substeps a steady_clock read per phase is noise, at millions of faces it
// would BE the profile. The counters are therefore incremented by loop extents
// (`+= n`) at loop boundaries rather than by ++ inside the loop, which also
// keeps them correct without atomics when the flux loop goes parallel.
//
// SERIAL-PATH ONLY. `n_fv_alg_*` are incremented inside solveAlgebraicNode,
// which is serial today (plan Phase 3d proposes parallelizing it). If that
// lands, these become per-thread accumulators or they become wrong.
// ---------------------------------------------------------------------------

inline double sec_fv_census      = 0.0;  // censusDt (Courant min-reduction)
inline double sec_fv_flux        = 0.0;  // computeFluxes (active-face sweep)
inline double sec_fv_nodesolve   = 0.0;  // relaxNodeFluxes + solveAlgebraicNode
inline double sec_fv_positivity  = 0.0;  // limitPositivity
inline double sec_fv_cellupdate  = 0.0;  // updateCells (incl. depth inversion)
inline double sec_fv_nodeupdate  = 0.0;  // updateNodes
inline double sec_fv_refreshdep  = 0.0;  // refreshDepths (full-mesh inversion)
inline double sec_fv_savestate   = 0.0;  // saveState (11-vector snapshot)
inline double sec_fv_restore     = 0.0;  // restoreState (snapshot + refreshDepths)
inline double sec_fv_structref   = 0.0;  // refreshStructFlows (solver side)
inline double sec_fv_bndcallback = 0.0;  // Router::refreshFvBoundaryFlows (engine side)
inline double sec_fv_rebuild     = 0.0;  // rebuildActiveLists (halo growth)
inline double sec_fv_reconstruct = 0.0;  // reconstructState (MUSCL slopes)
inline double sec_fv_ltsfire     = 0.0;  // fireFaces (LTS face pass)
inline double sec_fv_settle      = 0.0;  // settleAccumulators
inline double sec_fv_tier        = 0.0;  // assignTiers

inline long n_fv_substep     = 0;  // accepted substeps
inline long n_fv_census      = 0;  // censusDt calls
inline long n_fv_census_face = 0;  // faces visited by all censuses
inline long n_fv_invert      = 0;  // depthOfArea calls (by loop extent)
inline long n_fv_savestate   = 0;  // saveState calls
inline long n_fv_restore     = 0;  // ROLLBACKS — the rate saveState pays for
inline long n_fv_structref   = 0;  // substep structure refreshes
inline long n_fv_alg_visit   = 0;  // solveAlgebraicNode calls that got past the
                                   // fixed-head early-out
inline long n_fv_alg_passthru= 0;  // ...of which took the degree-2 shortcut.
                                   // passthru/visit is the fraction Phase 3c
                                   // is trying to raise
inline long n_fv_alg_solve   = 0;  // ...of which ran the root solve
inline long n_fv_alg_resid   = 0;  // residual(h) evaluations inside them
inline long n_fv_alg_flux    = 0;  // computeFaceFlux calls made from residuals

/** @brief Zeroes the FV phase accumulators. Called from Router::initFv. */
inline void reset_fv() noexcept {
    sec_fv_census = sec_fv_flux = sec_fv_nodesolve = sec_fv_positivity = 0.0;
    sec_fv_cellupdate = sec_fv_nodeupdate = sec_fv_refreshdep = 0.0;
    sec_fv_savestate = sec_fv_restore = 0.0;
    sec_fv_structref = sec_fv_bndcallback = 0.0;
    sec_fv_rebuild = sec_fv_reconstruct = sec_fv_ltsfire = 0.0;
    sec_fv_settle = sec_fv_tier = 0.0;
    n_fv_substep = n_fv_census = n_fv_census_face = n_fv_invert = 0;
    n_fv_savestate = n_fv_restore = n_fv_structref = 0;
    n_fv_alg_visit = n_fv_alg_passthru = 0;
    n_fv_alg_solve = n_fv_alg_resid = n_fv_alg_flux = 0;
}

/**
 * @brief One machine-scrapeable line for the FV phase split.
 * @details Same `[PERF-FV] key=value` contract as dump_load(); emitted from
 *          close() when OPENSWMM_PERF is set and the FV solver actually ran.
 *          `total` is the sum of the bracketed phases, NOT the router step —
 *          the difference between the two is unattributed time and is itself
 *          the useful signal when a phase is missing.
 */
inline void dump_fv() noexcept {
    const double total = sec_fv_census + sec_fv_flux + sec_fv_nodesolve
                       + sec_fv_positivity + sec_fv_cellupdate + sec_fv_nodeupdate
                       + sec_fv_refreshdep + sec_fv_savestate + sec_fv_restore
                       + sec_fv_structref + sec_fv_bndcallback + sec_fv_rebuild
                       + sec_fv_reconstruct + sec_fv_ltsfire + sec_fv_settle
                       + sec_fv_tier;
    std::fprintf(stderr,
        "[PERF-FV] step=%.4f total=%.4f "
        "census=%.4f flux=%.4f nodesolve=%.4f positivity=%.4f "
        "cellupdate=%.4f nodeupdate=%.4f refreshdepths=%.4f "
        "savestate=%.4f restore=%.4f structrefresh=%.4f bndcallback=%.4f "
        "rebuild=%.4f reconstruct=%.4f ltsfire=%.4f settle=%.4f tier=%.4f "
        "n.substep=%ld n.census=%ld n.censusface=%ld n.invert=%ld "
        "n.savestate=%ld n.rollback=%ld n.structrefresh=%ld "
        "n.algvisit=%ld n.algpassthru=%ld n.algsolve=%ld "
        "n.algresidual=%ld n.algfaceflux=%ld "
        "passthru_frac=%.4f rollback_frac=%.4f "
        "algresid_per_solve=%.2f algflux_per_solve=%.2f\n",
        sec_1d_step, total,
        sec_fv_census, sec_fv_flux, sec_fv_nodesolve, sec_fv_positivity,
        sec_fv_cellupdate, sec_fv_nodeupdate, sec_fv_refreshdep,
        sec_fv_savestate, sec_fv_restore, sec_fv_structref, sec_fv_bndcallback,
        sec_fv_rebuild, sec_fv_reconstruct, sec_fv_ltsfire, sec_fv_settle,
        sec_fv_tier,
        n_fv_substep, n_fv_census, n_fv_census_face, n_fv_invert,
        n_fv_savestate, n_fv_restore, n_fv_structref,
        n_fv_alg_visit, n_fv_alg_passthru, n_fv_alg_solve,
        n_fv_alg_resid, n_fv_alg_flux,
        (n_fv_alg_visit > 0)
            ? static_cast<double>(n_fv_alg_passthru) / static_cast<double>(n_fv_alg_visit) : 0.0,
        (n_fv_savestate > 0)
            ? static_cast<double>(n_fv_restore) / static_cast<double>(n_fv_savestate) : 0.0,
        (n_fv_alg_solve > 0)
            ? static_cast<double>(n_fv_alg_resid) / static_cast<double>(n_fv_alg_solve) : 0.0,
        (n_fv_alg_solve > 0)
            ? static_cast<double>(n_fv_alg_flux) / static_cast<double>(n_fv_alg_solve) : 0.0);
}

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

/**
 * @brief ScopedTimer that reads the clock only when OPENSWMM_PERF is set.
 *
 * @details ScopedTimer is used at per-routing-step and per-load-phase call
 *          sites, where two unconditional clock reads are free. The FV phase
 *          timers are not in that regime: a 30 h / 5 s run is ~21,600 routing
 *          steps, each of which can carry hundreds of substeps, each carrying a
 *          dozen phases. At ~20 ns a read that is seconds of pure instrument on
 *          a run that has nothing to do with profiling. `enabled()` is a cached
 *          static bool, so the gated form costs a predicted branch instead.
 *
 */
struct GatedTimer {
    double* acc = nullptr;
    std::chrono::steady_clock::time_point t0;
    explicit GatedTimer(double& a) noexcept {
        if (enabled()) { acc = &a; t0 = std::chrono::steady_clock::now(); }
    }
    ~GatedTimer() noexcept {
        if (acc)
            *acc += std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - t0).count();
    }
    GatedTimer(const GatedTimer&) = delete;
    GatedTimer& operator=(const GatedTimer&) = delete;
};

/// Adds @p n to a counter only when profiling is on, so the counters cost the
/// same predicted branch as the timers and never appear in a release profile.
inline void count(long& c, long n = 1) noexcept {
    if (enabled()) c += n;
}

} // namespace openswmm::perf
