// SPDX-License-Identifier: Apache-2.0

/**
 * @file      FlowTracer.hpp
 * @brief     Flow Tracer interface and supporting types.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <openswmm/engine/openswmm_trace.h>
#include <string>
#include <vector>

namespace openswmm::trace {
struct Node {
    std::string id;
    int type{}, flags{};
};
struct Link {
    std::string id;
    int from{}, to{}, type{};
    double length{};
};
/** @brief Own topology, SI hydraulic averages and steady-flow trace preparation state.
 * The C API validates topology/options and supplies aligned arrays. Direct C++
 * callers must preserve those invariants: valid endpoint indices, finite lengths,
 * na.size()==nodes.size(), la.size()==links.size(), and valid positive solver
 * tolerances/iteration limits. Serialize all access to one tracer; methods can
 * mutate error/state and invoke synchronous callbacks. No internal transaction
 * or C++ exception boundary is supplied; C wrappers catch most analysis exceptions. */
class FlowTracer {
public:
    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<SWMM_TraceNodeAverage> na;
    std::vector<SWMM_TraceLinkAverage> la;
    SWMM_TraceOptions options{};
    SWMM_TraceInfo info{};
    std::string error;
    bool prepared = false;
    /** @brief Derive directions, residence/travel times and flags from installed SI averages.
     * @return OK after setting prepared=true, INVALID after recording invalid averages.
     * Resets derived flags/outgoing fields while walking; failure can leave partial
     * derived state and does not itself reset an already true prepared flag.
     * Conduits use length/absolute velocity for travel seconds, other link types zero.
     * Storage residence uses volume/outgoing-withdrawal-loss; flagged exchange/ponding
     * forces approximate/unknown residence as applicable. Unknown times are NaN. */
    int derive();
    /** live: scan the whole periods already written to an output that may still be
     *  growing (no footer needed, no cache; appends are fine, truncation fails). */
    /** @param path Required nonempty NUL-terminated output filename. Completed mode
     * requires successful finalized output; live mode accepts complete periods already
     * written and ignores later appended periods during this scan. IDs are matched to
     * topology by exact names; counts and reporting data must agree.
     * @param cache Optional nonempty HDF5 sidecar destination/source. Completed mode
     * can reuse an exact cache key; a requested new cache requires a nonempty digest.
     * @param digest Optional caller-supplied content fingerprint, incorporated with
     * topology and selected thresholds. This method does not calculate/verify the digest.
     * @param cb Optional synchronous progress callback; nonzero requests cancellation
     * at polling points. Must not mutate/reenter this tracer or throw across a C boundary.
     * @param user Borrowed opaque callback context, never owned or retained afterward.
     * @param live Disable cache/digest and accept a growing file; final size may grow
     * but shrink rejects the scan. Completed mode rejects size/write-time changes.
     * @return Trace status for preparation, IO, mismatch, invalid data or cancellation.
     * Starts unprepared and clears error; computes time-averaged SI data by piecewise
     * linear positive integrals, with a synthetic one-second averaging weight for one
     * report and reported duration zero. Earlier failures can leave partial averages.
     * After derive succeeds, a missing cache digest/write failure can return an error
     * while prepared remains true; cancellation at the pre-cache poll clears it.
     * Final 100% callback cancellation is ignored. Filesystem/allocation exceptions propagate. */
    int prepare(const char* path, const char* cache, const char* digest, SWMM_TraceProgress cb,
                void* user, bool live = false);
    /** @brief Solve steady upstream/downstream fractions and conditional travel-time moments.
     * @param direction Valid SWMM_TRACE_UPSTREAM or SWMM_TRACE_DOWNSTREAM; direct callers
     * must validate it because any other integer follows the downstream branches.
     * @param seed Valid zero-based node index; no bounds check occurs in this method.
     * @param[out] nv Required writable array of at least nodes.size() trace values.
     * @param[out] lv Writable array of at least links.size(); null only for zero links.
     * @param[out] summary Reset initially, then filled with reachability, terminal
     * fractions, solver residual and partial SI-volume balance diagnostics.
     * @param cb Optional synchronous cancellation/progress callback; must not reenter
     * or mutate this tracer.
     * @param user Borrowed callback context, passed through unchanged.
     * @return OK, CANCELLED or SOLVER after SCC-local sparse solves. Requires prepared
     * coherent averages/topology/options; the C wrapper enforces readiness and capacities.
     * Closed reachable circulation has NaN traversal ratios/unknown times and explicit
     * trapped terminal fractions. Unknown-time edges contribute fraction but not known
     * time moments. Solver failure can leave summary or output arrays partially updated;
     * there is no rollback. Final completion callback return is ignored.
     * Ratios are expected traversal amounts and can exceed one in circulating open
     * networks; time fields use seconds and partial balance fields cubic metres. */
    int estimate(int direction, int seed, SWMM_TraceValue* nv, SWMM_TraceValue* lv,
                 SWMM_TraceSummary& summary, SWMM_TraceProgress cb, void* user);
    /** @brief Build a versioned identity string for cached hydraulic averages.
     * @param digest Optional NUL-terminated fingerprint; null contributes an empty string.
     * @return Independent key including trace versions, flow/velocity/reversal thresholds
     * and length-prefixed topology IDs/types/flags/endpoints/lengths. Solver tolerance/
     * iteration count are deliberately absent; no hash/content read is performed here. */
    std::string cacheKey(const char* digest) const;
    /** @brief Load exact-key HDF5 report metadata and seven-column node/link averages.
     * @param path Required NUL-terminated sidecar filename when HDF5 is enabled.
     * @param key Exact expected cache identity; no digest verification occurs here.
     * @return False without HDF5 or on normal IO/key/dimension/average validation failure.
     * HDF5 reads are serialized by a process-local cache mutex and error printing is
     * suppressed. Successful load fills info/averages and derive() sets prepared.
     * Normal failure clears prepared/error but may retain partially replaced metadata/
     * averages; allocation exceptions propagate without that cleanup guarantee. */
    bool readCache(const char* path, const std::string& key);
    /** @brief Write owned SI averages through an HDF5 staging sidecar and replace the target.
     * @param path Required NUL-terminated destination filename when HDF5 is enabled.
     * @param key Exact identity bytes written with six metadata values and flattened
     * seven-column node/link averages. Caller supplies coherent prepared state.
     * @return False without HDF5 or for a normal create/write/flush/rename failure.
     * A unique sibling .part timestamp/serial file is closed then renamed/replaced;
     * normal failure attempts staging removal. Cache mutex serializes this implementation.
     * Exceptions can leave staging files and propagate; no filesystem durability guarantee
     * beyond the platform rename/flush calls is supplied. Tracer state is not modified. */
    bool writeCache(const char* path, const std::string& key);
};
} // namespace openswmm::trace
