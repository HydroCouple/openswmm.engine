/**
 * @file OperatorSnapshotState.hpp
 * @brief Per-instance storage backing the SWMM_OperatorSnapshot C struct.
 *
 * @details Owns buffers for the snapshot, callback registration, and optional
 *          iteration-history ring buffer.  Allocated once during startup or
 *          first callback registration.  No per-step dynamic allocations.
 *
 * @ingroup engine_core
 */

#ifndef OPENSWMM_OPERATOR_SNAPSHOT_STATE_HPP
#define OPENSWMM_OPERATOR_SNAPSHOT_STATE_HPP

#include "../../../include/openswmm/engine/openswmm_operator_snapshot.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace openswmm {

/**
 * @brief Per-engine-instance operator snapshot state.
 *
 * @details Stores the callback, snapshot struct, and optional iteration
 *          residual ring buffer.  All memory is pre-allocated at init.
 */
class OperatorSnapshotState {
public:
    OperatorSnapshotState() {
        std::memset(&snap_, 0, sizeof(snap_));
    }

    // -----------------------------------------------------------------------
    // Callback management
    // -----------------------------------------------------------------------

    void setCallback(SWMM_OperatorSnapshotCallback cb, void* ud) noexcept {
        callback_ = cb;
        user_data_ = ud;
    }

    bool hasCallback() const noexcept { return callback_ != nullptr; }

    SWMM_OperatorSnapshotCallback callback() const noexcept { return callback_; }
    void* userData() const noexcept { return user_data_; }

    // -----------------------------------------------------------------------
    // Snapshot access
    // -----------------------------------------------------------------------

    SWMM_OperatorSnapshot&       snapshot()       noexcept { return snap_; }
    const SWMM_OperatorSnapshot& snapshot() const noexcept { return snap_; }

    bool hasBeenPopulated() const noexcept { return populated_; }
    void markPopulated() noexcept { populated_ = true; }
    void resetPopulated() noexcept { populated_ = false; }

    bool pollEnabled() const noexcept { return poll_enabled_; }
    void enablePoll() noexcept { poll_enabled_ = true; }

    /// Reset all transient state (call on engine close/reopen).
    void resetTransientState() noexcept {
        populated_ = false;
        poll_enabled_ = false;
    }

    // -----------------------------------------------------------------------
    // Iteration history (optional ring buffer)
    // -----------------------------------------------------------------------

    /**
     * @brief Enable iteration history with the given capacity.
     *
     * @param max_iters  Maximum iterations to record per substep (0 = disable).
     * @param n_nodes    Number of nodes in the model.
     */
    void enableIterHistory(int max_iters, int n_nodes) {
        if (max_iters <= 0) {
            iter_history_.clear();
            iter_history_.shrink_to_fit();
            iter_cap_ = 0;
            iter_count_ = 0;
            n_nodes_hist_ = 0;
            return;
        }
        iter_cap_ = max_iters;
        n_nodes_hist_ = n_nodes;
        // Flat buffer: [max_iters * n_nodes]
        iter_history_.assign(
            static_cast<size_t>(max_iters) * static_cast<size_t>(n_nodes), 0.0);
        iter_count_ = 0;
    }

    bool hasIterHistory() const noexcept { return iter_cap_ > 0; }

    /**
     * @brief Record one iteration's per-node residual.
     *
     * @param iter       Zero-based iteration index within the current substep.
     * @param residuals  Per-node residual array of size n_nodes_hist_.
     */
    void recordResidual(int iter, const double* residuals) {
        if (iter < 0 || iter >= iter_cap_ || !residuals) return;
        auto offset = static_cast<size_t>(iter) * static_cast<size_t>(n_nodes_hist_);
        std::memcpy(iter_history_.data() + offset, residuals,
                     static_cast<size_t>(n_nodes_hist_) * sizeof(double));
        if (iter + 1 > iter_count_) iter_count_ = iter + 1;
    }

    void resetIterCount() noexcept { iter_count_ = 0; }

    int iterCount() const noexcept { return iter_count_; }
    int iterCap()   const noexcept { return iter_cap_; }

    /**
     * @brief Get recorded residuals for one iteration.
     *
     * @param iter    Zero-based Picard iteration.
     * @param out     Output buffer of size n_nodes_hist_.
     * @param n       Size of out buffer.
     * @returns true on success, false if out-of-range.
     */
    bool getResidual(int iter, double* out, int n) const {
        if (iter < 0 || iter >= iter_count_ || !out) return false;
        int count = (n < n_nodes_hist_) ? n : n_nodes_hist_;
        auto offset = static_cast<size_t>(iter) * static_cast<size_t>(n_nodes_hist_);
        std::memcpy(out, iter_history_.data() + offset,
                     static_cast<size_t>(count) * sizeof(double));
        return true;
    }

    // -----------------------------------------------------------------------
    // Staging buffers for non-contiguous solver data
    // -----------------------------------------------------------------------

    /**
     * @brief Resize staging buffers for snapshot fields that need scatter
     *        from AoS or std::vector<bool> (which has no .data()).
     */
    void resizeStaging(int n_nodes, int n_links, int n_conduits = 0) {
        bypassed_buf_.assign(static_cast<size_t>(n_links), 0);
        node_converged_buf_.assign(static_cast<size_t>(n_nodes), 0);
        node_surcharged_buf_.assign(static_cast<size_t>(n_nodes), 0);
        sumdqdh_buf_.assign(static_cast<size_t>(n_nodes), 0.0);
        flow_class_buf_.assign(static_cast<size_t>(n_links), 0);
        link_type_buf_.assign(static_cast<size_t>(n_links), 0);
        if (n_conduits > 0) {
            dps_slot_area_buf_.assign(static_cast<size_t>(n_conduits), 0.0);
            dps_surcharge_head_buf_.assign(static_cast<size_t>(n_conduits), 0.0);
            dps_preissmann_num_buf_.assign(static_cast<size_t>(n_conduits), 0.0);
        }
    }

    uint8_t* bypassedBuf()       noexcept { return bypassed_buf_.data(); }
    uint8_t* nodeConvergedBuf()  noexcept { return node_converged_buf_.data(); }
    uint8_t* nodeSurchargedBuf() noexcept { return node_surcharged_buf_.data(); }
    double*  sumdqdhBuf()        noexcept { return sumdqdh_buf_.data(); }
    int8_t*  flowClassBuf()      noexcept { return flow_class_buf_.data(); }
    int*     linkTypeBuf()       noexcept { return link_type_buf_.data(); }
    double*  dpsSlotAreaBuf()    noexcept { return dps_slot_area_buf_.data(); }
    double*  dpsSurchargeHeadBuf() noexcept { return dps_surcharge_head_buf_.data(); }
    double*  dpsPreissmannNumBuf() noexcept { return dps_preissmann_num_buf_.data(); }

private:
    SWMM_OperatorSnapshotCallback callback_  = nullptr;
    void*                         user_data_ = nullptr;
    SWMM_OperatorSnapshot         snap_{};
    bool                          populated_ = false;
    bool                          poll_enabled_ = false;

    // Staging buffers for AoS → flat array scatter
    std::vector<uint8_t> bypassed_buf_;
    std::vector<uint8_t> node_converged_buf_;
    std::vector<uint8_t> node_surcharged_buf_;
    std::vector<double>  sumdqdh_buf_;
    std::vector<int8_t>  flow_class_buf_;
    std::vector<int>     link_type_buf_;
    std::vector<double>  dps_slot_area_buf_;
    std::vector<double>  dps_surcharge_head_buf_;
    std::vector<double>  dps_preissmann_num_buf_;

    // Iteration history ring buffer
    std::vector<double> iter_history_;
    int iter_cap_      = 0;     ///< Max iterations to record
    int iter_count_    = 0;     ///< Iterations recorded this substep
    int n_nodes_hist_  = 0;     ///< Nodes per residual vector
};

}  // namespace openswmm

#endif  // OPENSWMM_OPERATOR_SNAPSHOT_STATE_HPP
