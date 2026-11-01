// SPDX-License-Identifier: Apache-2.0
// A1.3: qualify the scheduler with the existing hydraulic and solute kernels,
// without adding a hook to the production marcher. Run via CTest or pass a
// directory to retain the partial checkpoint and the event trace.
#include "2d/subsurface/GwClock.hpp"
#include "2d/subsurface/LidNodeExchange.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/InertialEdges.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "data/NodeData.hpp"
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>

using namespace openswmm::twoD;
using Archive = openswmm::detail::CompletedSourceArchive;
namespace fs = std::filesystem;
namespace {
void require(bool b, const char* why) {
    if (!b)
        throw std::runtime_error(why);
}
double sum(const std::vector<double>& a) {
    return std::accumulate(a.begin(), a.end(), 0.);
}

struct Rig final : GwClock::Owner {
    MeshData mesh;
    InertialEdges edges;
    SolverOptions2D opts;
    SurfaceStateData surf;
    SubsurfaceConfig config;
    SubsurfaceSolver gw;
    openswmm::NodeData node;
    std::optional<GwClock> clock;
    double node_mass = 2, surface_water = 1, surface_mass = .5, node_concentration = 0;
    double neighbour_scale = 0, water0 = 0, mass0 = 0, water_error = 0, mass_error = 0;
    double check_end = 0, node_end = 0;
    bool paired_cell;
    std::vector<double> face_end;
    using Trace =
        std::array<double, 4>; // cell id (or -1-face), start, end, external gather permitted
    std::vector<Trace> trace;

    explicit Rig(GwClock::Options options = {0, 1, 32, 1}, bool paired = true)
        : paired_cell(paired) {
        // Three triangles around a centroid, with one outer triangle on each
        // side. The first two stiff cells each share a face with slow cell 2.
        mesh.resize_vertices(7);
        mesh.vx = {0, -1, 1, 0, 0, 2, -2};
        mesh.vy = {0, -1, -1, 1, -2, 1, 1};
        mesh.vz.assign(7, 4);
        mesh.resize_triangles(6);
        mesh.set_triangle(0, 0, 1, 2);
        mesh.set_triangle(1, 0, 2, 3);
        mesh.set_triangle(2, 0, 3, 1);
        mesh.set_triangle(3, 2, 1, 4);
        mesh.set_triangle(4, 3, 2, 5);
        mesh.set_triangle(5, 1, 3, 6);
        buildMeshTopology(mesh);
        edges.build(mesh);
        require(edges.ne == 6, "unexpected six-cell geometry");
        opts.lts_tiers = 2;
        opts.len_1d_to_2d = opts.vol_1d_to_2d = 1;
        surf.resize(6, 7);
        config.options.authored = true;
        config.options.gw_et = "NONE";
        GwAquiferRow r;
        r.Ks = .0001;
        r.zs = 4;
        r.theta_s = .45;
        r.theta_r = .1;
        r.alpha = 2;
        r.hg0 = 1;
        r.closure_set = true;
        r.closure = GwClosure::CLOSED_FORM;
        config.rows.push_back(r);
        r.scope = 2;
        r.cell = 2;
        r.closure = GwClosure::ENSLAVED;
        config.rows.push_back(r); // only the saturated diffusion bound on the watched neighbour
        r.cell = 3;
        r.hg0 = 1.2;
        r.closure = GwClosure::CLOSED_FORM;
        config.rows.push_back(r); // a nonzero Darcy packet is pending on the slow side at the save
        config.node_beds.push_back({0, 0, .02, .1, .1});
        std::vector<std::string> warnings;
        auto err = gw.initialize(mesh, edges, opts, {}, 1, config, warnings);
        if (!err.empty())
            throw std::runtime_error(err);
        RowLayoutLite rows;
        rows.n_species = rows.n_pollut = 1;
        rows.names = {"tracer"};
        gw.initTransport(rows, nullptr, {}, warnings);
        auto& tr = gw.transport();
        for (int i = 0; i < 6; ++i) {
            tr.sat_mass[i] = (.1 + i * .1) * gw.satVolume(i);
            tr.unsat_mass[i] = .2 * gw.unsatVolume(i);
            tr.init_mass[0] += tr.sat_mass[i] + tr.unsat_mass[i];
        }
        if (paired)
            require(gw.configureSourceReceipts({5}).empty(),
                    "paired surface intake failed to bind");
        node.resize(1);
        node.volume[0] = 10;
        gw.setNodeRowConc(&node_concentration, 1);
        gw.refreshDtCell(mesh, edges);
        // Script conservative caps for the stiff pair and amplify the
        // sensitivity test by placing the measured neighbour bound just above
        // a rung threshold. Its subsequent changes come from the real stencil.
        neighbour_scale = 9.00001 / gw.state().dt_cell[2];
        require(neighbour_scale < 1, "fixture must not relax the physical stability limit");
        std::vector<double> bounds;
        for (int i = 0; i < 6; ++i)
            bounds.push_back(stability(i));
        std::vector<GwClock::Face> faces;
        for (int e = 0; e < edges.ne; ++e)
            faces.emplace_back(edges.cL[e], edges.cR[e]);
        clock.emplace(options, bounds, std::move(faces));
        face_end.resize(edges.ne);
        water0 = water();
        mass0 = mass();
    }

    double stability(int i) const override {
        const double physical = gw.state().dt_cell[i];
        if (i == 2)
            return neighbour_scale * physical;
        return std::min(physical, i == 0 ? .26 : i == 1 ? .13 : 18.);
    }

    // The gate uses the existing shared tier interface as a selection mask.
    // A simple mesh has exactly one face with both requested endpoints on
    // tier 1. This O(N) test adapter is not the production batching strategy.
    void select(int i, int j = -1) {
        auto& d = gw.state().dt_cell;
        const auto saved = d;
        std::fill(d.begin(), d.end(), 1.);
        d[i] = 2;
        if (j >= 0)
            d[j] = 2;
        gw.assignTiers(1, 2);
        d = saved;
    }

    void faces(std::span<const GwClock::Interval> intervals) override {
        for (auto [e, start, end] : intervals) {
            require(face_end[e] == start && end > start, "duplicate or missing face interval");
            select(edges.cL[e], edges.cR[e]);
            gw.fireGwFaces(1, end - start);
            face_end[e] = end;
            trace.push_back({double(-1 - e), start, end, -1});
            audit();
        }
    }

    void cells(std::span<const GwClock::Interval> intervals,
               std::optional<double> through) override {
        for (auto [i, start, end] : intervals) {
            require(gw.cellCompletedTime(i) == start, "cell clock detached from kernel");
            require(!through ||
                        (*through <= end && (*through == check_end || *through == clock->time())),
                    "external gather before producer check");
            for (int p = edges.cell_ptr[i]; p < edges.cell_ptr[i + 1]; ++p)
                require(face_end[edges.cell_edge[p]] == end, "cell fired before incident face");
            select(i);
            auto& s = gw.state();
            auto& tr = gw.transport();
            const double exported0 = tr.node_out_mass[0];
            // Expose lateral packets to every kernel firing. Keep the external
            // water AND its tracer paired until a check endpoint. The fixture
            // has no external channels except this node and the surface intake.
            const bool mask_node = i == 0 && (!through || node_end > *through);
            double nw = 0, nm = 0;
            if (mask_node) {
                std::swap(nw, s.nacc[0]);
                std::swap(nm, tr.nacc_mass[0]);
            }
            if (paired_cell && i == 5)
                gw.fireClockIntakeCell(i, end, through.value_or(-1.), surf);
            else
                gw.fireGwCells(1, end - start, surf, start, end);
            if (mask_node) {
                std::swap(nw, s.nacc[0]);
                std::swap(nm, tr.nacc_mass[0]);
                require(nw == 0 && nm == 0, "unexpected external production");
            }
            node_mass += tr.node_out_mass[0] - exported0;
            trace.push_back({double(i), start, end, through.value_or(-1.)});
            audit();
        }
        gw.refreshDtCell(mesh, edges);
    }

    double water() const {
        // liveStorage includes lateral/surface packets, but not nacc: a
        // positive node receipt is already in the 1D owner's credited volume.
        return gw.state().liveStorage() - gw.state().nacc[0] + node.volume[0] + surface_water;
    }
    double mass() const {
        const auto& t = gw.transport();
        return sum(t.sat_mass) + sum(t.unsat_mass) + sum(t.sacc_L) + sum(t.sacc_R) +
               sum(t.xacc_from_surface) + sum(t.xacc_to_surface) + sum(t.nacc_mass) + node_mass +
               surface_mass;
    }
    void audit() {
        const double w = std::abs(water() - water0), m = std::abs(mass() - mass0);
        require(w <= 1e-12 && m <= 1e-12, "water or species conservation failed");
        water_error = std::max(water_error, w);
        mass_error = std::max(mass_error, m);
    }
    void surfacePacket() {
        require(clock->time() == .1875, "intake must be published at its producer endpoint");
        const double v = .001, m = .0004;
        surface_water -= v;
        surface_mass -= m;
        // Model a completed mesh donor interval. The stamped water and its
        // actual native mass must survive a save while their GW owner is held.
        SurfaceIntakeRequest q{SurfaceDonorKind::MESH, "cell:5", -1, 5, 1, .1, v, v};
        gw.bookInfiltrationFromSurface(5, v);
        gw.bookInfiltrationMass(5, 0, m);
        gw.retainSourceReceipt({q, 0, .1875, v, {m}});
        audit();
    }
    double step(double end, int direction = 0, bool dry = false, bool finish = false) {
        double vol = 0;
        check_end = end;
        if (direction) {
            require(gw.state().nacc[0] == 0 && gw.transport().nacc_mass[0] == 0,
                    "fixture expects no previous node packet before sampling");
            node_end = end;
            node.depth[0] = direction < 0 ? 3 : .1;
            node_concentration = node_mass / node.volume[0];
            gw.resetNodeExchangeVolumes();
            const double m0 = gw.transport().nacc_mass[0];
            gw.sampleNodeExchange(&node, end - clock->time(), end);
            vol = gw.nodeExchangeVolumes()[0];
            require(direction * vol > 0, "node contact did not reverse as prescribed");
            node.volume[0] += vol;
            node_mass -= gw.transport().nacc_mass[0] - m0;
            audit();
        }
        clock->check(end, *this, dry, finish);
        audit();
        return vol;
    }
    void archive(Archive& a) {
        gw.checkpoint(a, false);
        gw.checkpointIndependentClock(a, *clock);
        a(node.volume[0], node.depth[0], node_mass, node_concentration, surface_water, surface_mass,
          neighbour_scale, water0, mass0, water_error, mass_error, check_end, node_end);
        a.fixed(face_end);
        a(surf.infil_rate, trace);
    }
    std::string bytes() {
        Archive a;
        archive(a);
        return a.bytes();
    }
    void restore(std::string bytes) {
        Archive a(std::move(bytes));
        archive(a);
        a.finish();
    }
};

// Timing contracts isolated from hydraulic tolerances. A real-valued check
// interval and off-grid first check expose rounding and accidental tail firings.
struct ClockProbe final : GwClock::Owner {
    double accuracy_limit = 1e30;
    std::vector<double> accuracy_limits;
    std::vector<double> bounds;
    std::vector<GwClock::Interval> fired;
    double tightening_at = -1;
    void faces(std::span<const GwClock::Interval>) override {}
    void cells(std::span<const GwClock::Interval> cells, std::optional<double>) override {
        for (auto c : cells) {
            fired.push_back(c);
            if (c.id == 0 && c.end == tightening_at)
                bounds[1] = .13;
        }
    }
    double stability(int i) const override { return bounds[i]; }
    double accuracy(int i) const override {
        return accuracy_limits.empty() ? accuracy_limit : accuracy_limits[i];
    }
};
void timingContracts() {
    ClockProbe physical;
    physical.bounds = {3.7};
    GwClock ten({0, 1, 10, 3}, physical.bounds, {}), sixty({0, 1, 60, 3}, physical.bounds, {});
    require(ten.margin() == sixty.margin() && ten.rung(0) == sixty.rung(0) && ten.rung(0) == -1,
            "nonbinding ceiling changed the physical check allowance or rung");
    require(ten.producerStep() == 1. && sixty.producerStep() == 1.,
            "physical stiffness changed the producer cadence below its authored base");
    GwClock fine_ceiling({0, 1, .0625, 3}, std::array{1000.}, {});
    require(fine_ceiling.producerStep() == .0625,
            "authored fine ceiling did not refine external delivery");
    GwClock base_ceiling({0, 1, 1, 3}, std::array{1000.}, {});
    require(base_ceiling.producerStep() == base_ceiling.period(base_ceiling.rung(0)),
            "ceiling refinement and its producer cadence disagree");
    ClockProbe accuracy;
    accuracy.bounds = {1000};
    accuracy.accuracy_limit = 4.75;
    GwClock accuracy_ten({0, 1, 10, 3}, accuracy.bounds, {}, std::array{4.75});
    GwClock accuracy_sixty({0, 1, 60, 3}, accuracy.bounds, {}, std::array{4.75});
    require(accuracy_ten.rung(0) == 0 && accuracy_sixty.rung(0) == 0,
            "nonbinding ceiling changed the accuracy period");
    ClockProbe deadlines;
    deadlines.bounds = {1e30, 1e30};
    deadlines.accuracy_limits = {3., 1e30};
    GwClock guarded({0, 1, 4, 60}, deadlines.bounds, {}, deadlines.accuracy_limits);
    guarded.check(.1875, deadlines);
    guarded.land(std::array{0}, deadlines);
    require(guarded.rung(0) == 0 && guarded.rung(1) == 1,
            "deadline fixture did not select its coarse periods");
    Archive pending_deadlines;
    guarded.checkpoint(pending_deadlines);
    GwClock restored_deadlines({0, 1, 4, 60}, deadlines.bounds, {}, deadlines.accuracy_limits);
    Archive restore_deadlines(pending_deadlines.bytes());
    restored_deadlines.checkpoint(restore_deadlines);
    restore_deadlines.finish();
    require(guarded.needsBaseCheck() && restored_deadlines.needsBaseCheck(),
            "restoring an occupied base rung lost its producer check cap");
    auto restored_owner = deadlines;
    guarded.check(4.125, deadlines);
    restored_deadlines.check(4.125, restored_owner);
    // Cell 0 has the earliest nominal due time but a later hard limit.
    // Checking only that cell would overrun cell 1's four-second ceiling.
    require(guarded.completed(1) > 0 && guarded.completed(1) <= 4.125,
            "a later nominal deadline bypassed an earlier hard limit");
    Archive a, b;
    guarded.checkpoint(a);
    restored_deadlines.checkpoint(b);
    require(a.bytes() == b.bytes() && deadlines.fired.size() == restored_owner.fired.size() &&
                std::equal(deadlines.fired.begin(), deadlines.fired.end(),
                           restored_owner.fired.begin(),
                           [](auto x, auto y) {
                               return x.id == y.id && x.start == y.start && x.end == y.end;
                           }),
            "restoring a held clock changed hard-deadline ordering or events");
    // Complete legacy v1 record: reject its scheduling policy even though
    // options, topology, shape, and stored physical bound all match.
    Archive legacy;
    std::string legacy_tag = "Independent groundwater clock v1";
    double old_epoch = 0, old_base = 1, maximum = 4, surface = 60, time = 0, bound = 1000;
    std::uint64_t count = 1;
    int rung = 1;
    std::vector<GwClock::Face> topology;
    std::vector<double> face_times;
    legacy(legacy_tag, old_epoch, old_base, maximum, surface);
    legacy.fixed(topology);
    legacy(time, count, time, bound, rung);
    legacy.fixed(face_times);
    GwClock policy({0, 1, 4, 60}, std::array{1000.}, {});
    const auto state_bytes = [](GwClock& clock) {
        Archive saved;
        clock.checkpoint(saved);
        return saved.bytes();
    };
    const auto before = state_bytes(policy);
    bool legacy_rejected = false;
    try {
        Archive input(legacy.bytes());
        policy.checkpoint(input);
    } catch (const std::runtime_error&) {
        legacy_rejected = true;
    }
    require(legacy_rejected && state_bytes(policy) == before,
            "legacy scheduling policy accepted or changed the current clock");
    ClockProbe accurate;
    accurate.bounds = {3600};
    accurate.accuracy_limit = 1e-9;
    GwClock target({0, 1, 3600, .05}, accurate.bounds, {}, std::array{1e-9});
    require(target.rung(0) == 0 && target.committedBound(0) == 3600,
            "accuracy became a stiff physical stability bound");
    require(target.needsBaseCheck() && !target.hasFineCells(),
            "accuracy base rung did not request a producer check");
    target.check(1., accurate, true);
    require(accurate.fired.size() == 1 && target.completed(0) == 1.,
            "base-floor accuracy did not advance at the base check");
    ClockProbe p;
    p.bounds = {3600};
    GwClock coarse({0, 1, 3600, .05}, p.bounds, {});
    require(coarse.rung(0) == 11, "incorrect admissible coarse rung");
    require(!coarse.needsBaseCheck(), "slow cells shortened the producer check cadence");
    for (int i = 0; i < 144000; ++i)
        coarse.check(std::fma(double(i), .05, .025), p);
    coarse.check(7200, p, false, true);
    require(p.fired.size() == 4, "surface checks forced coarse firings");
    for (int j = 0; j < 3; ++j) {
        const double due = (j + 1) * 2048.;
        require(p.fired[j].end > due && p.fired[j].end <= due + .05,
                "coarse firing did not land at the first check after its deadline");
    }
    require(p.fired.back().end == 7200, "final partial interval was lost");
    ClockProbe n;
    n.bounds = {.13, 2.};
    n.tightening_at = 1.125;
    GwClock nearby({0, 1, 32, 1}, n.bounds, {{0, 1}});
    nearby.check(.875, n);
    nearby.check(1.75, n);
    const auto first =
        std::find_if(n.fired.begin(), n.fired.end(), [](auto c) { return c.id == 1; });
    require(first != n.fired.end() && first->end == 1.25 && nearby.completed(1) == 1.75,
            "an overdue coarse neighbour ignored a new stiff bound");
    ClockProbe stiff;
    stiff.bounds = {120};
    GwClock limited({0, 1, 1, 60}, stiff.bounds, {});
    limited.check(60, stiff);
    require(stiff.fired.size() == 120 && limited.completed(0) == 60,
            "small GW ceiling incorrectly restricted a 60-s surface interval");
    ClockProbe many;
    // Twelve occupied rungs, a non-binary base and a nonzero epoch. Exact
    // GW-only times must advance even when division rounds just below a tick.
    constexpr double epoch = .125, base = .1, margin = .0625;
    for (int k = -3; k <= 8; ++k)
        many.bounds.push_back(std::ldexp(base, k) + (k < 0 ? 0 : margin));
    GwClock ladder({epoch, base, 64, margin}, many.bounds, {});
    for (int i = 0; i < 12; ++i)
        require(ladder.rung(i) == i - 3, "missing test rung");
    ladder.check(epoch + 27, many, true);
    for (int i = 0; i < 12; ++i) {
        const double period = std::ldexp(base, i - 3);
        require(ladder.completed(i) == std::fma(std::floor(27 / period), period, epoch),
                "nonbinary ladder lost its phase");
    }
    bool rejected = false;
    try {
        nearby.check(3., n);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected && nearby.time() == 1.75 && n.fired.back().end == 1.75,
            "overlong check accepted or left partial owner changes");
    std::cout << "144000 off-grid surface checks: 3 coarse deadlines plus final landing; "
                 "tightened overdue neighbour subcycled at its next fine deadline.\n";
}

// An inert forcing knot does not generate water. Decay with a one-second
// half-life makes any prematurely adopted tracer measurable even when the
// total (including reaction loss) still closes. Compare with actual delivery
// of two different native concentrations at their two physical endpoints.
void knotIntakes() {
    Rig independent, oracle;
    for (Rig* r : {&independent, &oracle}) {
        GwResolvedSource marker;
        marker.cells = {{5, 1}};
        marker.flow.times = {0, 1, 2};
        marker.flow.values = {0, 0, 0};
        r->gw.setSources({marker});
        r->gw.transport().decay[5] = std::log(2.);
        r->select(5);
    }
    auto book = [](Rig& r, double start, double v, double m) {
        SurfaceIntakeRequest q{SurfaceDonorKind::MESH, "cell:5", -1, 5, 1, .1, v, v};
        r.surface_water -= v;
        r.surface_mass -= m;
        r.gw.bookInfiltrationFromSurface(5, v);
        r.gw.bookInfiltrationMass(5, 0, m);
        r.gw.retainSourceReceipt({q, start, start + 1, v, {m}});
    };
    book(independent, 0, .0002, .0001);
    book(independent, 1, .001, .0004);
    independent.gw.fireClockIntakeCell(5, 2, 2, independent.surf);
    book(oracle, 0, .0002, .0001);
    oracle.gw.fireGwCells(1, 1, oracle.surf, 0, 1);
    book(oracle, 1, .001, .0004);
    oracle.gw.fireGwCells(1, 1, oracle.surf, 1, 2);
    for (Rig* r : {&independent, &oracle})
        require(std::abs(r->water() - r->water0) < 1e-12 &&
                    std::abs(r->mass() + r->gw.transport().lost_reaction[0] - r->mass0) < 1e-12,
                "source knot ledger did not close");
    require(independent.gw.state().hg[5] == oracle.gw.state().hg[5] &&
                independent.gw.state().hu[5] == oracle.gw.state().hu[5] &&
                independent.gw.transport().sat_mass[5] == oracle.gw.transport().sat_mass[5] &&
                independent.gw.transport().unsat_mass[5] == oracle.gw.transport().unsat_mass[5] &&
                independent.gw.transport().lost_reaction[0] ==
                    oracle.gw.transport().lost_reaction[0] &&
                independent.gw.pendingSourceReceipts(5).empty(),
            "source knot borrowed a later packet");
    std::cout
        << "Two dated intakes across a forcing knot: heads, native mass and decay match explicit delivery.\n";
}

// Joint LID beds are a water/optional-temperature profile in the actual
// engine: they cannot be attached to the native pollutant fixture. Isolate a
// water-only reviewed Richards receipt and drive the same stiff-cell clock.
void lidReceipt() {
    Rig mesh;
    struct Bed final : GwClock::Owner {
        Rig& geometry;
        SubsurfaceSolver gw;
        double initial;
        explicit Bed(Rig& r) : geometry(r) {
            std::vector<std::string> warnings;
            auto err = gw.initialize(r.mesh, r.edges, r.opts, {}, 1, r.config, warnings);
            require(err.empty() && gw.configureLidReceipts({0}).empty(),
                    "water-only LID binding failed");
            initial = gw.state().liveStorage();
            auto bounds = gw.state().dt_cell;
            std::fill(gw.state().dt_cell.begin(), gw.state().dt_cell.end(), 1.);
            gw.state().dt_cell[0] = 2;
            gw.assignTiers(1, 2);
            gw.state().dt_cell = std::move(bounds);
        }
        double stability(int) const override { return .26; }
        void faces(std::span<const GwClock::Interval> e) override {
            require(e.empty(), "unexpected LID face");
        }
        void cells(std::span<const GwClock::Interval> c, std::optional<double> through) override {
            for (auto [id, start, end] : c) {
                require(id == 0 && gw.cellCompletedTime(0) == start, "missed LID firing");
                gw.fireGwCells(1, end - start, geometry.surf, start, end);
                const bool paid = end >= .875;
                require(paid == (gw.pendingBedUp(0) == 0) &&
                            (!paid || (through && *through >= .875)),
                        "LID withdrawal settled at the wrong check");
                require(std::abs(gw.state().liveStorage() + gw.pendingLidWater() + .001 - initial) <
                            1e-12,
                        "LID water inventory did not close");
            }
        }
    } bed(mesh);
    RichardsBedReceipt packet;
    packet.node = packet.bed = packet.cell = 0;
    packet.requested_up = packet.reserved_up = packet.accepted_up = .001;
    const auto error = bed.gw.retainLidReceipts(0, .875, {packet});
    if (!error.empty())
        throw std::runtime_error(error);
    GwClock clock({0, 1, 2, 1}, std::array{.26}, {});
    clock.check(.34375, bed);
    clock.check(.875, bed);
    require(bed.gw.state().led_node == .001 && bed.gw.nodeRefunds() == 0,
            "LID receipt lost or replayed");
    std::cout
        << "Water-only Richards withdrawal: reserved across stiff ticks, settled once at 0.875 s.\n";
}

void run(const fs::path& out) {
    timingContracts();
    knotIntakes();
    lidReceipt();
    fs::create_directories(out);
    Rig long_step({0, 1, 4, 60});
    long_step.step(.1875);
    long_step.surfacePacket();
    const auto held = long_step.bytes();
    Rig long_restore({0, 1, 4, 60});
    long_restore.restore(held);
    require(long_restore.bytes() == held, "held long-gap checkpoint changed state");
    long_step.step(7.125, -1);
    long_restore.step(7.125, -1);
    require(long_step.bytes() == long_restore.bytes(), "hard deadlines changed after restore");
    require(long_step.gw.pendingSourceReceipts(5).empty(), "earlier completed intake was starved");
    require(std::any_of(long_step.trace.begin(), long_step.trace.end(),
                        [](auto e) { return e[0] == 5 && e[2] == 2 && e[3] == .1875; }) &&
                long_step.clock->completed(0) == 7.125 && long_step.gw.state().nacc[0] == 0,
            "numerical check did not separate the old intake from the current node packet");
    std::cout
        << "Long coupled gap: old paired intake gathered at 2 s; new node packet held until 7.125 s.\n";
    // Ordinary bulk checkpoints omit per-cell times; the companion clock
    // record must restore those too, independently of the paired-source case.
    Rig bulk({0, 1, 32, 1}, false), bulk_restore({0, 1, 32, 1}, false);
    bulk.step(.34375);
    bulk_restore.restore(bulk.bytes());
    bulk.step(.71875);
    bulk_restore.step(.71875);
    require(bulk.bytes() == bulk_restore.bytes(), "bulk kernel lost held times on restore");
    Rig continuous, saved;
    const auto prefix = [](Rig& r) {
        require(r.clock->rung(0) == -2 && r.clock->rung(1) == -3 && r.clock->rung(2) == 3,
                "fixture failed to occupy its intended rungs");
        r.step(.1875);
        r.surfacePacket();
        r.step(.34375, -1);
        require(r.gw.state().hg[0] > 1 && r.clock->rung(2) < 3 && r.clock->completed(2) == 0,
                "neighbour head rise did not tighten the unfired slow cell");
        require(r.gw.state().xacc_from_surface[5] > 0 && r.gw.transport().xacc_from_surface[5] > 0,
                "partial checkpoint must have pending surface water and tracer");
        const auto& pending = r.gw.pendingSourceReceipts(5);
        require(pending.size() == 1 && pending[0].end == .1875 && pending[0].mass[0] == .0004,
                "held intake lost its timestamp or paired mass");
        require(r.gw.state().eacc_L != std::vector<double>(r.edges.ne) ||
                    r.gw.state().eacc_R != std::vector<double>(r.edges.ne),
                "no pending face side");
    };
    prefix(continuous);
    prefix(saved);
    const auto state = saved.bytes();
    const auto path = out / "partial.chk";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(state.data(), state.size());
        require(bool(f), "checkpoint write failed");
    }
    std::ifstream f(path, std::ios::binary);
    Rig restarted;
    restarted.restore(std::string{std::istreambuf_iterator<char>(f), {}});
    require(restarted.bytes() == state, "checkpoint read changed the partial state");
    auto suffix = [](Rig& r) {
        r.step(.71875, 1);
        for (double t : {1.0625, 1.5, 2.25, 3., 4., 5., 6., 7., 8., 8.125})
            r.step(t);
        r.step(8.125, 0, false, true); // end, including not-yet-due owners
    };
    suffix(continuous);
    suffix(saved);
    suffix(restarted);
    require(continuous.gw.pendingSourceReceipts(5).empty(),
            "completed cell did not adopt its intake");
    require(saved.bytes() == continuous.bytes() && restarted.bytes() == continuous.bytes(),
            "save/read or restore changed the trajectory");
    std::ofstream trace(out / "events.csv");
    trace << "cell_or_minus_one_minus_face,start_s,end_s,external_completed_through_or_minus_one\n"
          << std::setprecision(17);
    for (const auto& e : continuous.trace)
        trace << e[0] << ',' << e[1] << ',' << e[2] << ',' << e[3] << '\n';
    require(bool(trace), "trace write failed");
    std::cout << "Six-cell gate: " << continuous.trace.size()
              << " face/cell intervals; max water residual " << continuous.water_error
              << " m3; max tracer residual " << continuous.mass_error
              << "; write/read and restored trajectories byte-identical.\n";

    Rig a, b, c;
    for (int t = 1; t <= 30; ++t)
        a.step(t, 0, true, t == 30);
    for (int t = 10; t <= 30; t += 10)
        b.step(t, 0, true, t == 30);
    c.step(30, 0, true, true);
    require(a.bytes() == b.bytes() && a.bytes() == c.bytes(), "GW-only clocks depend on windows");
    std::cout << "GW-only 1/10/30 s windows: byte-identical state and event trace.\n";
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw std::invalid_argument("Expected retained evidence directory");
        run(argv[1]);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "GW clock gate failed: " << e.what() << '\n';
        return 1;
    }
}
