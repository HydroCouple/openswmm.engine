/**
 * @file HotStartManager.hpp
 * @brief Hot start file manager — in-memory representation + I/O.
 *
 * @details `HotStartManager` provides the C++ implementation of the hot start
 *          API declared in `include/openswmm/engine/openswmm_hotstart.h`.
 *
 *          ### Binary file format
 *
 *          **V1** (legacy — no infiltration/GW state):
 *          ```
 *          Offset  Size   Field
 *          ------  ----   -----
 *          0       16     Magic "OPENSWMM_HS_V1\0"
 *          16      4      Version: uint32_t = 1
 *          ...            (nodes, links as before)
 *                         Per-subcatch: [uint32 name_len, char name[],
 *                                        double runoff, double gwater]
 *          EOF-4   4      CRC32: uint32_t
 *          ```
 *
 *          **V2** (Gap #54 — includes infiltration model state and GW zone state):
 *          ```
 *          0       16     Magic "OPENSWMM_HS_V1\0"  (same magic, version byte changes)
 *          16      4      Version: uint32_t = 2
 *          20      8      Timestamp (Unix seconds): int64_t
 *          28      8      Simulation time at save (decimal days): double
 *          36      8      Start date (OADate): double
 *          44      8      End date (OADate): double
 *          52      4      CRS string length: uint32_t
 *          56      n      CRS string (null-terminated)
 *          56+n    4      Node count: uint32_t
 *          ...     ...    Per-node: [uint32 name_len, char name[], double depth,
 *                                    double head, double volume]
 *                         Link count: uint32_t
 *                         Per-link: [uint32 name_len, char name[], double flow,
 *                                    double depth, double volume]
 *                         Subcatch count: uint32_t
 *                         Per-subcatch V2: [uint32 name_len, char name[],
 *                                           double runoff, double gwater,
 *                                           uint32_t infil_model,
 *                                           double infil[6],
 *                                           double gw_theta,
 *                                           double gw_lower_depth]
 *          EOF-4   4      CRC32 of all preceding bytes: uint32_t
 *          ```
 *
 *          ### Threading
 *
 *          `HotStartFile` instances are not thread-safe. The C API wraps each
 *          handle with a raw pointer cast; callers are responsible for
 *          not sharing handles across threads.
 *
 * @see include/openswmm/engine/openswmm_hotstart.h — C API
 * @see Legacy reference: src/solver/hotstart.c — swmm_saveHotstart()
 * @ingroup engine_hotstart
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_HOT_START_MANAGER_HPP
#define OPENSWMM_ENGINE_HOT_START_MANAGER_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace openswmm {

struct SimulationContext;
namespace runoff     { class RunoffSolver; }
namespace groundwater { class GWSolver; }

// ============================================================================
// Per-object state records
// ============================================================================

/** @brief Node hydraulic state at hot-start save time. */
struct HotStartNodeRecord {
    std::string id;
    double      depth  = 0.0;
    double      head   = 0.0;
    double      volume = 0.0;
};

/** @brief Link hydraulic state at hot-start save time. */
struct HotStartLinkRecord {
    std::string id;
    double      flow   = 0.0;
    double      depth  = 0.0;
    double      volume = 0.0;
};

/** @brief Subcatchment state at hot-start save time. */
struct HotStartSubcatchRecord {
    std::string id;
    double      runoff = 0.0;
    double      gwater = 0.0;

    // Gap #54: infiltration model state (V2 only; -1 = not present / V1 file)
    int         infil_model   = -1;
    double      infil[6]      = {0, 0, 0, 0, 0, 0};  ///< flat 6-elem state (model-dependent)

    // Gap #54: groundwater zone state (V2 only; gw_theta < 0 = not present)
    double      gw_theta      = -1.0; ///< upper zone moisture content
    double      gw_lower_depth = 0.0; ///< lower zone depth (ft)
};

// ============================================================================
// File header (in-memory mirror of the on-disk header)
// ============================================================================

/** @brief In-memory representation of the OPENSWMM_HS_V1 header. */
struct HotStartHeader {
    uint32_t version    = 1;
    int64_t  timestamp  = 0;    ///< Unix epoch seconds at save time
    double   sim_time   = 0.0;  ///< Simulation elapsed time (decimal days)
    double   start_date = 0.0;  ///< options.start_date (OADate (days since 12/30/1899))
    double   end_date   = 0.0;  ///< options.end_date (OADate (days since 12/30/1899))
    std::string crs;            ///< CRS string (may be empty)
};

// ============================================================================
// HotStartFile — opaque handle (cast to void* in C API)
// ============================================================================

/**
 * @brief In-memory hot start file data.
 *
 * @details Created by `HotStartManager::save()` or `HotStartManager::open()`.
 *          The C API casts `HotStartFile*` to/from `void* SWMM_HotStart`.
 *
 * @ingroup engine_hotstart
 */
struct HotStartFile {
    HotStartHeader                    header;
    std::vector<HotStartNodeRecord>   nodes;
    std::vector<HotStartLinkRecord>   links;
    std::vector<HotStartSubcatchRecord> subcatches;

    std::string               path;      ///< File path (for flush-on-close)
    bool                      dirty = false; ///< True if set_*() was called

    std::vector<std::string>  warnings;  ///< Populated by HotStartManager::apply()

    // -----------------------------------------------------------------------
    // Modification helpers
    // -----------------------------------------------------------------------

    /** @brief Set depth for a stored node. Returns false if id not found. */
    bool set_node_depth  (const std::string& id, double v);
    /** @brief Set head for a stored node. Returns false if id not found. */
    bool set_node_head   (const std::string& id, double v);
    /** @brief Set flow for a stored link. Returns false if id not found. */
    bool set_link_flow   (const std::string& id, double v);
    /** @brief Set depth for a stored link. Returns false if id not found. */
    bool set_link_depth  (const std::string& id, double v);
    /** @brief Set runoff for a stored subcatchment. Returns false if id not found. */
    bool set_subcatch_runoff(const std::string& id, double v);
};

// ============================================================================
// HotStartManager — static methods (no state; acts as a utility namespace)
// ============================================================================

/**
 * @brief Static utility class for hot start file operations.
 *
 * @details All operations on the binary file are handled here.
 *          The `HotStartFile` struct is the data carrier;
 *          `HotStartManager` provides the I/O logic.
 *
 * @ingroup engine_hotstart
 */
class HotStartManager {
public:
    HotStartManager() = delete;

    // -----------------------------------------------------------------------
    // Save
    // -----------------------------------------------------------------------

    /**
     * @brief Capture current engine state and write to a hot start file (V1 format).
     *
     * @param ctx   Simulation context (must be RUNNING or ENDED).
     * @param path  Output file path.
     * @returns Heap-allocated `HotStartFile*` on success (caller owns), or
     *          nullptr on I/O error (error description in `last_io_error`).
     */
    static HotStartFile* save(const SimulationContext& ctx,
                              const std::string& path);

    /**
     * @brief Capture current engine state including infiltration and GW (V2 format).
     *
     * @details Gap #54: extends V1 save with per-subcatchment infiltration model
     *          state (6-element flat array) and GW zone state (theta, lower_depth).
     *          Writes version=2 header.
     *
     * @param ctx      Simulation context (must be RUNNING or ENDED).
     * @param runoff   RunoffSolver providing infiltration state (may be nullptr).
     * @param gw       GWSolver providing GW zone state (may be nullptr).
     * @param path     Output file path.
     */
    static HotStartFile* save(const SimulationContext& ctx,
                              const runoff::RunoffSolver* runoff,
                              const groundwater::GWSolver* gw,
                              const std::string& path);

    // -----------------------------------------------------------------------
    // Open
    // -----------------------------------------------------------------------

    /**
     * @brief Read and validate a hot start file.
     *
     * @details Validates magic, version, and CRC32. Loads all records into
     *          memory.
     *
     * @param path  File path.
     * @returns Heap-allocated `HotStartFile*` on success, nullptr on error.
     */
    static HotStartFile* open(const std::string& path);

    // -----------------------------------------------------------------------
    // Apply
    // -----------------------------------------------------------------------

    /**
     * @brief Apply hot start records to a simulation context.
     *
     * @details For each record in the hot start file, the matching object
     *          (by string ID) in `ctx` is updated. Objects in the hot start
     *          that are absent from `ctx` generate warnings — they do not fail.
     *
     * @param hs       Hot start file (from save() or open()).
     * @param ctx      Target simulation context (must be INITIALIZED).
     * @param warn_cb  Optional callback for per-missing-object warnings.
     * @returns Number of missing-object warnings generated (0 = perfect match).
     */
    static int apply(HotStartFile& hs,
                     SimulationContext& ctx,
                     std::function<void(const std::string&)> warn_cb = {});

    /**
     * @brief Apply hot start records including infiltration and GW state.
     *
     * @details Gap #54: extended apply that also restores infiltration model
     *          state and GW zone state when the file is V2 format.
     *
     * @param hs       Hot start file (from save() or open()).
     * @param ctx      Target simulation context (must be INITIALIZED).
     * @param runoff   RunoffSolver to restore infiltration state into (may be nullptr).
     * @param gw       GWSolver to restore GW zone state into (may be nullptr).
     * @param warn_cb  Optional callback for per-missing-object warnings.
     * @returns Number of missing-object warnings generated.
     */
    static int apply(HotStartFile& hs,
                     SimulationContext& ctx,
                     runoff::RunoffSolver* runoff,
                     groundwater::GWSolver* gw,
                     std::function<void(const std::string&)> warn_cb = {});

    // -----------------------------------------------------------------------
    // Flush (write-back modifications)
    // -----------------------------------------------------------------------

    /**
     * @brief If the file is dirty, rewrite it to disk.
     *
     * @returns true on success, false on I/O error.
     */
    static bool flush(HotStartFile& hs);

    // -----------------------------------------------------------------------
    // Last I/O error (thread-local; for error reporting to C API layer)
    // -----------------------------------------------------------------------

    /** @brief Description of the most recent I/O error (empty = none). */
    static const std::string& last_io_error() noexcept;

private:
    // -----------------------------------------------------------------------
    // Binary I/O helpers
    // -----------------------------------------------------------------------

    static bool write_file(const HotStartFile& hs, const std::string& path);
    static bool read_file (HotStartFile& hs, const std::string& path);

    // -----------------------------------------------------------------------
    // CRC32 (IEEE 802.3 polynomial, no external dependency)
    // -----------------------------------------------------------------------

    static uint32_t crc32(const uint8_t* data, std::size_t len) noexcept;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_HOT_START_MANAGER_HPP */
