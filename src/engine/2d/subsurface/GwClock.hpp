// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <functional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace openswmm::detail {
class CompletedSourceArchive;
}

namespace openswmm::twoD {

// Absolute dyadic clock shared by the production CPU marcher and the numerical gate.
// IDs are dense covered-cell IDs; faces are ordered by the supplied topology.
class GwClock {
public:
    struct Options {
        double epoch, base_step, max_step, surface_max_step;
    };
    using Face = std::pair<int, int>;
    struct Interval {
        int id;
        double start, end;
    };
    struct Owner {
        virtual ~Owner() = default;
        virtual void faces(std::span<const Interval> intervals) = 0;
        // Lateral sides are always available. If set, external_through is the
        // most recent completed producer check whose receipts may be consumed.
        // A numerical check inside a long surface step cannot see its arrivals.
        virtual void cells(std::span<const Interval> intervals,
                           std::optional<double> external_through) = 0;
        virtual double stability(int cell) const = 0;
        virtual double accuracy(int) const { return 1e30; }
        virtual void early(double, std::vector<int>&) {}
    };
    GwClock(Options options, std::span<const double> bounds, std::vector<Face> faces,
            std::span<const double> accuracy = {});

    // Ordinary coupled deadlines wait at most margin() for a producer check.
    // A long surface interval contains numerical GW checks when necessary.
    // GW-only intervals visit all deadlines exactly with no external packets.
    // finish lands unfinished cells; an ordinary check or snapshot does not.
    // Owner failures require rollback of the enclosing owner/clock transaction.
    void check(double end, Owner& owner, bool gw_only = false, bool finish = false,
               const std::function<void()>& publish = {});
    void land(std::span<const int> cells, Owner& owner);
    void refresh(Owner& owner);
    void checkpoint(detail::CompletedSourceArchive& archive);

    double time() const noexcept { return time_; }
    std::size_t cellCount() const noexcept { return cells_.size(); }
    double margin() const noexcept;
    bool hasFineCells() const noexcept { return !fine_.empty(); }
    bool needsBaseCheck() const noexcept { return hasFineCells() || base_cells_ != 0; }
    // An authored fine ceiling also refines external delivery. Physical
    // subcycles alone retain the base producer cadence.
    double producerStep() const;
    int rung(int cell) const { return cells_.at(cell).rung; }
    double committedBound(int cell) const { return cells_.at(cell).committed_bound; }
    double completed(int cell) const { return cells_.at(cell).last; }
    double completedFace(int face) const { return face_last_.at(face); }
    double period(int rung) const;

private:
    struct Cell {
        double last, committed_bound;
        int rung;
    };
    using Queue = std::set<std::pair<double, int>>;
    Options options_;
    double time_;
    std::vector<Cell> cells_;
    std::vector<Face> faces_;
    std::vector<std::vector<int>> incident_;
    std::vector<double> face_last_, due_, latest_;
    Queue fine_, coarse_, limits_;
    std::size_t base_cells_ = 0; // derived occupancy, rebuilt on restore

    int select(double bound, double accuracy = 1e30) const;
    double next(double after, int rung) const;
    void schedule(int id, int rung, double after);
    void event(double t, std::vector<int> ids, std::optional<double> external_through, Owner& owner,
               std::set<int>& landing);
};

} // namespace openswmm::twoD
