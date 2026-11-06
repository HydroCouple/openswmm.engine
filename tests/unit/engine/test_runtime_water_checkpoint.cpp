// SPDX-License-Identifier: Apache-2.0

/**
 * @file      test_runtime_water_checkpoint.cpp
 * @brief     Tests for runtime water checkpoint.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include "2d/coupling/RuntimeCouplingCheckpoint.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "core/SWMMEngine.hpp"
#include <openswmm/engine/openswmm_coupling.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <cstdlib>

using namespace openswmm::twoD;
namespace {
using Archive = openswmm::detail::CompletedSourceArchive;
struct Stores {
    RuntimeSources surface, groundwater;
    RuntimeForcings forcing;
};
/**
 * @brief Construct the fixed three-cell replay identity for the checkpoint fixture.
 * @return Owned binding with identity revision 1, groundwater mask {true,true,false} and completed
 *     time 5.375 seconds; remaining boundary fields use defaults.
 */
RuntimeWaterCheckpointBinding binding() {
    return {"model cells and replay driver revision 1", {true, true, false}, 5.375};
}
/**
 * @brief Construct a copied source prescription without inserting it into a store.
 *
 * @param[in] owner Provider owner copied into the row; empty leaves id unqualified.
 * @param[in] id Source identifier; qualified as provider/owner/id when owner is nonempty.
 * @param[in] cell Target zero-based cell index, unvalidated here.
 * @param[in] flow Signed volumetric source rate in m3/s, copied without validation.
 * @return Owned row with supplied owner/id/cell/flow and all other fields at RuntimeSource
 *     defaults.
 */
RuntimeSource source(std::string owner, std::string id, int cell, double flow) {
    RuntimeSource r;
    r.owner = owner;
    r.id = owner.empty() ? id : "provider/" + owner + "/" + id;
    r.cell = cell;
    r.flow = flow;
    return r;
}
/**
 * @brief Construct a copied forcing row with explicit channel, target and rate.
 *
 * @param[in] ch Runtime forcing channel, normally 1 rain, 2 evaporation or 3 paired infiltration;
 *     not validated.
 * @param[in] cell Zero-based receiver, or -1 for the uniform forcing row.
 * @param[in] rate Depth rate in m/s for the intended water forcing channels, inserted without
 *     validation.
 * @param[in] owner Provider owner copied into the row, default weather.
 * @return Owned row with selected fields set and remaining fields at RuntimeForcing defaults.
 */
RuntimeForcing forcing(int ch, int cell, double rate, std::string owner = "weather") {
    RuntimeForcing r;
    r.channel = ch;
    r.cell = cell;
    r.rate = rate;
    r.owner = owner;
    return r;
}
/**
 * @brief Build mixed live, expired and cleared water prescriptions with nonzero receipts.
 *
 * Uses colliding short source IDs under distinct owners, disabled expired history, a cleared well
 * row, uniform and per-cell rain, ET mode 2 and infiltration receipt history. Does not apply flows,
 * advance time or invoke checkpoint capture.
 * @return Owned surface/groundwater/forcing stores with lookup structures rebuilt for three cells.
 */
Stores stores() {
    Stores s;
    auto a = source("pump", "shared", 0, -.02);
    a.receipt.requested = -.5;
    a.receipt.applied = .2;
    a.receipt.water_in = .5;
    a.receipt.water_out = .3;
    a.receipt.last_flow = -.1;
    a.receipt.last_dt = 1;
    a.until = 12.75;
    s.surface.replace(a);
    s.surface.replace(source("other", "shared", 0, .01));
    auto expired = source("", "expired", 1, .01);
    expired.enabled = false;
    expired.until = 2.5;
    expired.receipt.requested = expired.receipt.applied = expired.receipt.water_in = .01;
    expired.receipt.last_flow = .01;
    expired.receipt.last_dt = 1;
    s.surface.replace(expired);
    s.surface.rebuild(3);
    s.groundwater.replace(source("well", "in", 1, .02));
    s.groundwater.replace(source("well", "out", 0, -.02));
    s.groundwater.clear("provider/well/out");
    s.groundwater.rebuild(3);
    s.forcing.replace(forcing(1, -1, 1e-6));
    s.forcing.replace(forcing(1, 0, 2e-6));
    auto et = forcing(2, -1, 1e-7);
    et.mode = 2;
    s.forcing.replace(et);
    auto infiltration = forcing(3, 0, 1e-6, "soil");
    infiltration.receipt.requested = 2;
    infiltration.receipt.applied = infiltration.receipt.water_out = .5;
    infiltration.receipt.last_flow = .25;
    infiltration.receipt.last_dt = 1;
    s.forcing.replace(infiltration);
    s.forcing.rebuild(3);
    return s;
}
/**
 * @brief Capture copied fixture stores using the selected completed-time/model binding.
 *
 * Delegates semantic validation/encoding to RuntimeWaterCheckpoint::capture and records an
 * EXPECT_EQ diagnostic; does not throw solely for a nonempty returned error.
 *
 * @param[in] s Stable source/forcing stores compatible with b; unchanged.
 * @param[in] b Replay/model/receiver/time binding, defaulting to binding().
 * @return Owned encoded bytes after capture; may be empty when the nonfatal success expectation
 *     records an error.
 */
std::string capture(const Stores& s, const RuntimeWaterCheckpointBinding& b = binding()) {
    std::string bytes;
    EXPECT_EQ(RuntimeWaterCheckpoint::capture(b, s.surface, s.groundwater, s.forcing, bytes), "");
    return bytes;
}
/**
 * @brief Restore copied fixture prescriptions under a required binding.
 *
 * Forwards only source/forcing stores; no live physical water or external driver state is restored.
 *
 * @param[in] bytes Encoded same-platform water checkpoint to decode/validate.
 * @param[in,out] s Target stores replaced on success and preserved on a reported restore error.
 * @param[in] b Required model/receiver/completed-time binding, defaulting to binding().
 * @return Empty string for success, otherwise the production restore diagnostic.
 */
std::string restore(const std::string& bytes, Stores& s,
                    const RuntimeWaterCheckpointBinding& b = binding()) {
    return RuntimeWaterCheckpoint::restore(b, bytes, s.surface, s.groundwater, s.forcing);
}
// Decode/rewrite explicit wire fields to exercise semantic rejection with a
// correct checksum. These helpers are never a production checkpoint writer.
/**
 * @brief Compute the fixture's reflected CRC-32 over all supplied bytes.
 *
 * A checksum only; does not validate wire structure or protect against semantic mutations.
 *
 * @param[in] bytes Binary payload, including embedded zero bytes; may be empty.
 * @return CRC-32 using polynomial 0xedb88320 with initial/final all-bit complement.
 */
uint32_t crc(const std::string& bytes) {
    uint32_t v = ~uint32_t(0);
    for (unsigned char c : bytes) {
        v ^= c;
        for (int k = 0; k < 8; ++k)
            v = (v >> 1) ^ ((v & 1) ? 0xedb88320u : 0u);
    }
    return ~v;
}
struct Wire {
    std::string magic, identity, payload;
    uint32_t version = 0, endian = 0, integer = 0, real = 0, checksum = 0;
    std::vector<bool> cells;
    double time = 0;
    /**
     * @brief Decode the original water-checkpoint envelope for deliberate test mutation.
     *
     * Reads framing fields and requires archive completion. Archive decoding/trailing-byte failures
     * propagate; does not itself verify checksum, identity, version semantics or reconstruct
     * prescription stores.
     *
     * @param[in] bytes Owned complete same-platform fixture envelope in the explicit field order; moved
     *     into the decoding archive.
     */
    explicit Wire(std::string bytes) {
        Archive a(std::move(bytes));
        fields(a);
        a.finish();
    }
    /**
     * @brief Serialize or decode this explicit fixture envelope field sequence.
     *
     * Mutates this Wire when decoding and archive bytes when encoding; native ABI conventions apply.
     * Does not finish the archive or refresh checksum. Intended for the original no-boundary envelope
     * fixture, not a general production writer.
     *
     * @param[in,out] a CompletedSourceArchive in encode or decode mode; receives/reads magic, platform
     *     fields, identity, mask, time, payload and checksum.
     */
    void fields(Archive& a) {
        a(magic, version, endian, integer, real, identity, cells, time, payload, checksum);
    }
    /**
     * @brief Encode this fixture wire object with its currently stored checksum.
     *
     * Does not recalculate checksum or validate semantic compatibility. Archive/allocation errors
     * propagate; no production checkpoint capture is invoked.
     * @return Owned binary envelope with fields in the explicit original fixture order.
     */
    std::string bytes() {
        Archive a;
        fields(a);
        return a.bytes();
    }
};
/**
 * @brief Visit all six receipt scalars in the fixture payload order.
 *
 * Does not validate signs, conservation or finite values; archive errors propagate.
 *
 * @param[in,out] a Encoding or decoding archive, advanced through the receipt values.
 * @param[in,out] r Receipt read during encoding or assigned during decoding:
 *     requested/applied/in/out volumes m3, last flow m3/s and last dt seconds.
 */
void receipt(Archive& a, RuntimeReceipt& r) {
    a(r.requested, r.applied, r.water_in, r.water_out, r.last_flow, r.last_dt);
}
/**
 * @brief Visit source-domain and forcing rows in the original water payload order.
 *
 * Serializes source id/owner/cell/flow/until/enabled and forcing
 * channel/cell/mode/rate/until/enabled/owner, plus receipts. Does not serialize/rebuild lookup
 * caches or validate production semantics. Archive errors propagate.
 *
 * @param[in,out] a Encoding/decoding archive advanced through both source sequences then forcing
 *     sequence.
 * @param[in,out] s Stores read during encoding or populated during decoding; only rows and receipts
 *     are visited.
 */
void fields(Archive& a, Stores& s) {
    const auto visit = [&](auto& r) {
        a(r.id, r.owner, r.cell, r.flow, r.until, r.enabled);
        receipt(a, r.receipt);
    };
    a.sequence(s.surface.rows, visit);
    a.sequence(s.groundwater.rows, visit);
    a.sequence(s.forcing.rows, [&](auto& r) {
        a(r.channel, r.cell, r.mode, r.rate, r.until, r.enabled, r.owner);
        receipt(a, r.receipt);
    });
}
/**
 * @brief Decode explicit fixture payload rows without production semantic validation.
 *
 * Calls archive finish to reject trailing/truncated framing errors; archive exceptions propagate.
 * Intended only for controlled negative-test mutation.
 *
 * @param[in] bytes Complete binary payload in the original source/source/forcing field order.
 * @return Owned decoded stores; derived lookup arrays/maps remain unrebuilt defaults.
 */
Stores rows(const std::string& bytes) {
    Stores s;
    Archive a(bytes);
    fields(a, s);
    a.finish();
    return s;
}
/**
 * @brief Modify decoded prescription rows and rebuild the fixture envelope checksum.
 *
 * Does not validate the mutated rows; this permits checksum-correct semantic rejection tests.
 * Decode, callback and encode exceptions propagate; input bytes/stores are not modified.
 *
 * @param[in] bytes Valid original-layout fixture envelope/payload accepted by Wire and rows().
 * @param[in] change Nonempty synchronous mutation callback receiving a temporary decoded Stores
 *     object; must not retain references to it.
 * @return Owned re-encoded envelope with updated payload CRC, preserving other envelope fields.
 */
std::string mutate(const std::string& bytes, const std::function<void(Stores&)>& change) {
    Wire w(bytes);
    auto s = rows(w.payload);
    change(s);
    Archive a;
    fields(a, s);
    w.payload = a.bytes();
    w.checksum = crc(w.payload);
    return w.bytes();
}
/**
 * @brief Check rejected restoration leaves a target intact and a subsequent valid restore succeeds.
 *
 * Uses nonfatal expectations for rejection/preservation and a fatal assertion for recovery.
 * Compares re-encoded bytes exactly; fatal assertion returns from this helper, leaving caller
 * responsible for interpreting test status.
 *
 * @param[in] bad Checkpoint expected to fail under b.
 * @param[in] good Valid checkpoint matching the default binding(), used for the recovery stage.
 * @param[in,out] target Mutable stores captured before the failing restore and replaced by the
 *     valid recovery checkpoint.
 * @param[in] b Binding used only for the failing attempt; default binding() is used for
 *     before/after captures and recovery.
 */
void preservedFailure(const std::string& bad, const std::string& good, Stores& target,
                      const RuntimeWaterCheckpointBinding& b = binding()) {
    const auto before = capture(target);
    EXPECT_NE(restore(bad, target, b), "");
    EXPECT_EQ(capture(target), before);
    ASSERT_EQ(restore(good, target), "");
    EXPECT_EQ(capture(target), good);
}
}

TEST(RuntimeWaterCheckpoint, InitialEmptyDomainsAndDistinctTargetGuard) {
    auto b = binding();
    b.completed_time = 0;
    b.groundwater_cells = {false, false, false};
    Stores a, c;
    const auto bytes = capture(a, b);
    ASSERT_EQ(restore(bytes, c, b), "");
    EXPECT_EQ(c.surface.cells.size(), 3u);
    EXPECT_EQ(c.forcing.cells.size(), 3u);
    EXPECT_EQ(capture(c, b), bytes);
    std::string sentinel = "unchanged";
    EXPECT_NE(RuntimeWaterCheckpoint::capture(b, a.surface, a.surface, a.forcing, sentinel), "");
    EXPECT_EQ(sentinel, "unchanged");
    EXPECT_NE(RuntimeWaterCheckpoint::restore(b, bytes, c.surface, c.surface, c.forcing), "");
    EXPECT_EQ(capture(c, b), bytes);
}
TEST(RuntimeWaterCheckpoint, PreservesOwnershipOrderReceiptsClearsAndExpiry) {
    auto a = stores();
    const auto bytes = capture(a);
    Stores b;
    ASSERT_EQ(restore(bytes, b), "");
    EXPECT_EQ(capture(b), bytes);
    EXPECT_EQ(b.surface.find("provider/pump/shared", 0)->receipt.water_out, .3);
    EXPECT_EQ(b.surface.find("provider/other/shared", 0)->receipt.applied, 0);
    EXPECT_FALSE(b.surface.find("expired", 1)->enabled);
    EXPECT_FALSE(b.groundwater.find("provider/well/out", 0)->enabled);
    EXPECT_EQ(b.forcing.get(1, 0)->rate, 2e-6);
    EXPECT_EQ(b.forcing.get(1, 1)->rate, 1e-6);
    EXPECT_EQ(b.forcing.get(3, 0)->receipt.applied, .5);
    auto change = source("pump", "shared", 0, .03);
    b.surface.replace(change);
    EXPECT_EQ(b.surface.find(change.id, 0)->receipt.water_out, .3);
    EXPECT_TRUE(b.surface.clear("provider/other/shared"));
    b.surface.rebuild(3);
    b.surface.expire(12.75);
    EXPECT_TRUE(b.surface.find(change.id, 0)->enabled); // replacement is indefinite
}
TEST(RuntimeWaterCheckpoint, ValidatesCachesAndRebuildsExpiredForcingLookups) {
    auto a = stores();
    a.forcing.rows[1].until = 2.5;
    a.forcing.expire(binding().completed_time);
    EXPECT_FALSE(a.forcing.rows[1].enabled);
    EXPECT_EQ(a.forcing.cells[0][0], 1);
    const auto bytes = capture(a);
    Stores b;
    ASSERT_EQ(restore(bytes, b), "");
    EXPECT_EQ(b.forcing.cells[0][0], -1);
    EXPECT_EQ(b.forcing.get(1, 0)->cell, -1);
    EXPECT_EQ(capture(b), bytes);
    std::vector<std::function<void(Stores&)>> changes = {
        [](auto& s) {s.surface.index.clear(); },    [](auto& s) {s.surface.cells[0].clear(); },
        [](auto& s) {s.forcing.index.clear(); },    [](auto& s) {s.forcing.cells[0][0]=999; },
        [](auto& s) {s.forcing.uniform[0]=999; }, [](auto& s) {s.forcing.cells.clear(); },
        [](auto& s) {s.forcing.cells[0][0]=-1; }};
    for (std::size_t i = 0; i < changes.size(); ++i) {
        SCOPED_TRACE(i);
        auto bad = stores();
        changes[i](bad);
        std::string out = "unchanged";
        EXPECT_NE(RuntimeWaterCheckpoint::capture(binding(), bad.surface, bad.groundwater,
                                                  bad.forcing, out),
                  "");
        EXPECT_EQ(out, "unchanged");
    }
}
TEST(RuntimeWaterCheckpoint, RejectsChangedBindingWithoutChangingTargetsAndAllowsRetry) {
    const auto good = capture(stores());
    auto target = stores();
    std::vector<RuntimeWaterCheckpointBinding> changed(7, binding());
    changed[0].identity += " edited driver";
    changed[1].groundwater_cells[0] = false;
    changed[2].groundwater_cells.push_back(false);
    changed[3].completed_time += .125;
    changed[4].identity.clear();
    changed[5].completed_time = -1;
    changed[6].completed_time = std::numeric_limits<double>::quiet_NaN();
    for (std::size_t i = 0; i < changed.size(); ++i) {
        SCOPED_TRACE(i);
        preservedFailure(good, good, target, changed[i]);
    }
}
TEST(RuntimeWaterCheckpoint, RejectsSemanticMutationsAcrossAllStoresAndAllowsRetry) {
    const auto good = capture(stores());
    auto target = stores();
    const double nan = std::numeric_limits<double>::quiet_NaN(),
                 inf = std::numeric_limits<double>::infinity();
    std::vector<std::function<void(Stores&)>> changes = {
        [](auto& s) { s.surface.rows[0].id.clear(); },
        [](auto& s) { s.surface.rows[0].id = "boundary/0/0"; },
        [](auto& s) { s.surface.rows[0].owner = "different"; },
        [](auto& s) { s.surface.rows[0].owner = "a/b"; },
        [](auto& s) { s.surface.rows[0].owner = std::string("a\0b", 3); },
        [](auto& s) { s.surface.rows[0].id = "provider/pump/provider/illegal"; },
        [](auto& s) { s.surface.rows[0].cell = -1; },
        [](auto& s) { s.surface.rows[0].cell = 3; },
        [](auto& s) { s.surface.rows.push_back(s.surface.rows[0]); },
        [=](auto& s) { s.surface.rows[0].flow = nan; },
        [=](auto& s) { s.surface.rows[0].flow = inf; },
        [=](auto& s) { s.surface.rows[0].until = nan; },
        [](auto& s) { s.surface.rows[0].until = 0; },
        [](auto& s) { s.surface.rows[0].until = 5.375; },
        [](auto& s) { s.surface.rows[0].until = -1; },
        [](auto& s) { s.surface.rows[0].receipt.water_in = -1; },
        [](auto& s) { s.surface.rows[0].receipt.applied = 42; },
        [=](auto& s) { s.surface.rows[0].receipt.requested = nan; },
        [](auto& s) { s.surface.rows[0].receipt.last_dt = -1; },
        [](auto& s) { s.surface.rows[0].receipt.last_dt = 6; },
        [](auto& s) { s.surface.rows[0].receipt.last_dt = 0; },
        [](auto& s) { s.surface.rows[0].receipt.last_flow = 100; },
        [](auto& s) { s.groundwater.rows[0].cell = 2; },
        [](auto& s) { s.groundwater.rows.push_back(s.groundwater.rows[0]); },
        [](auto& s) { s.forcing.rows[0].channel = 4; },
        [](auto& s) { s.forcing.rows[0].channel = 0; },
        [](auto& s) { s.forcing.rows[0].cell = -2; },
        [](auto& s) { s.forcing.rows[0].mode = 3; },
        [](auto& s) { s.forcing.rows[0].mode = 0; },
        [](auto& s) { s.forcing.rows[0].rate = -1; },
        [=](auto& s) { s.forcing.rows[0].rate = inf; },
        [](auto& s) { s.forcing.rows.push_back(s.forcing.rows[0]); },
        [](auto& s) { s.forcing.rows[1].owner = "other weather"; },
        [](auto& s) { s.forcing.rows[3].cell = -1; },
        [](auto& s) { s.forcing.rows[3].cell = 2; },
        [](auto& s) { s.forcing.rows[3].mode = 2; },
        [](auto& s) { s.forcing.rows[3].receipt.requested = .1; },
        [](auto& s) { s.forcing.rows[3].receipt.applied = -.5; },
        [](auto& s) { s.forcing.rows[3].receipt.last_flow = -.25; },
        [](auto& s) { s.forcing.rows[0].receipt.last_dt = 1; },
        [](auto& s) { s.surface.rows[0].id += std::string(1, '\0'); }};
    for (std::size_t i = 0; i < changes.size(); ++i) {
        SCOPED_TRACE(i);
        preservedFailure(mutate(good, changes[i]), good, target);
    }
}
TEST(RuntimeWaterCheckpoint, RefusesUnqualifiedBoundaryHeatAndQualityOnCapture) {
    std::vector<std::function<void(Stores&)>> changes = {
        [](auto& s) { s.surface.rows[0].heat = 1; },
        [](auto& s) { s.groundwater.rows[0].heat = 1; },
        [](auto& s) { s.surface.rows[0].boundary_edge = 0; },
        [](auto& s) { s.groundwater.rows[0].boundary_head = 1; },
        [](auto& s) { s.surface.rows[0].concentrations = {0}; },
        [](auto& s) { s.groundwater.rows[0].rates = {0}; },
        [](auto& s) { s.surface.rows[0].receipt.heat_applied = 1; },
        [](auto& s) { s.groundwater.rows[0].receipt.requested_mass = {0}; },
        [](auto& s) { s.forcing.rows[0].concentrations = {0}; },
        [](auto& s) { s.forcing.rows[0].receipt.applied_mass = {0}; },
        [](auto& s) { s.forcing.rows[0].channel = 4; }};
    for (std::size_t i = 0; i < changes.size(); ++i) {
        SCOPED_TRACE(i);
        auto a = stores();
        changes[i](a);
        std::string out = "unchanged";
        EXPECT_NE(
            RuntimeWaterCheckpoint::capture(binding(), a.surface, a.groundwater, a.forcing, out),
            "");
        EXPECT_EQ(out, "unchanged");
    }
}
TEST(RuntimeWaterCheckpoint, RejectsMalformedWireCountsBooleanAndChecksumWithRetry) {
    const auto good = capture(stores());
    auto target = stores();
    for (std::size_t i = 0; i < good.size(); ++i) {
        SCOPED_TRACE(i);
        preservedFailure(good.substr(0, i), good, target);
    }
    preservedFailure(good + "trailing", good, target);
    std::vector<std::function<void(Wire&)>> changes = {
        [](auto& w) { w.magic += "bad"; },
        [](auto& w) { w.version++; },
        [](auto& w) { w.endian++; },
        [](auto& w) { w.integer++; },
        [](auto& w) { w.real++; },
        [](auto& w) { w.checksum++; },
        [](auto& w) { w.payload.back() ^= 1; },
        [](auto& w) {
            w.payload += "trailing";
            w.checksum = crc(w.payload);
        },
        [](auto& w) {
            const uint64_t n = ~uint64_t(0);
            w.payload.assign(reinterpret_cast<const char*>(&n), sizeof(n));
            w.checksum = crc(w.payload);
        },
        [](auto& w) {
            auto s = rows(w.payload);
            Archive prefix;
            uint64_t n = s.surface.rows.size();
            prefix(n);
            auto& r = s.surface.rows[0];
            prefix(r.id, r.owner, r.cell, r.flow, r.until);
            w.payload[prefix.bytes().size()] = 2;
            w.checksum = crc(w.payload);
        }};
    for (std::size_t i = 0; i < changes.size(); ++i) {
        SCOPED_TRACE(i);
        Wire w(good);
        changes[i](w);
        preservedFailure(w.bytes(), good, target);
    }
    preservedFailure(std::string(64 * 1024 * 1024 + 1, 'x'), good, target);
}

namespace {
struct EngineDelete {
    /**
     * @brief Close/delete a native engine owned by the fixture unique pointer.
     *
     * Ignores close status before delete. A non-null pointer must be exclusively owned and not used
     * afterward; the helper does not catch close/destructor exceptions.
     *
     * @param[in] p Owned native engine pointer; null is accepted as a no-op.
     */
    void operator()(openswmm::SWMMEngine* p) const {
        if (p) {
            p->close();
            delete p;
        }
    }
};
using Engine = std::unique_ptr<openswmm::SWMMEngine, EngineDelete>;
/**
 * @brief Advance a started coupling fixture to an exact elapsed-second boundary.
 *
 * Requires SWMM_OK and exact achieved-time equality through fatal GoogleTest assertions; failure
 * returns from the helper, with engine state left by the attempted advance.
 *
 * @param[in,out] e Non-null owning engine wrapper with a prepared/started coupling model; ownership
 *     remains unchanged.
 * @param[in] seconds Supported target model time in seconds at or after the current accepted
 *     boundary.
 */
void advance(Engine& e, double seconds) {
    double time = -1;
    ASSERT_EQ(swmm_coupling_advance_to(e.get(), seconds, &time), SWMM_OK);
    ASSERT_DOUBLE_EQ(time, seconds);
}
/**
 * @brief Copy live surface, groundwater and forcing prescription stores from a 2D engine.
 *
 * Does not advance, capture or mutate the engine; no reference into the engine survives in the
 * copied result.
 *
 * @param[in] e Non-null native engine wrapper with a prepared surface router and subsurface state.
 * @return Owned copies of runtime stores, including their current row/lookup/receipt state.
 */
Stores live(const Engine& e) {
    const auto& r = e->surfaceRouter2D();
    return {r.state().runtime_sources, r.subsurface().runtimeSources(), r.state().runtime_forcings};
}
/**
 * @brief Apply one version-one copied water coupling frame to a fixture engine.
 *
 * Constructs a stack frame with no boundary/species/heat rows and asserts SWMM_OK. Arrays/owner are
 * borrowed only during the synchronous API call; production API controls copied-store replacement.
 * Failure returns from this helper without a separate status.
 *
 * @param[in,out] e Non-null engine wrapper at a state accepting swmm_coupling_apply_frame.
 * @param[in] owner Non-null owner string valid for the synchronous frame application.
 * @param[in] sources Copied-source descriptors; vector size must fit int and any referenced
 *     descriptor strings remain alive through the call.
 * @param[in] forcings Copied-forcing descriptors; vector size must fit int and descriptor
 *     dependencies remain valid through the call.
 */
void frame(Engine& e, const char* owner, const std::vector<SWMM_CouplingSource>& sources,
           const std::vector<SWMM_CouplingForcing>& forcings) {
    const SWMM_CouplingFrame f{sizeof(f),       1, owner,   int(sources.size()),
                               sources.data(),  0, nullptr, int(forcings.size()),
                               forcings.data(), 0, nullptr};
    ASSERT_EQ(swmm_coupling_apply_frame(e.get(), &f), SWMM_OK);
}
}
TEST(RuntimeWaterCheckpoint, CopiedApiRecordsContinueAtSamePhysicalBoundaryAcrossThreeClosures) {
    const char* configured = std::getenv("SWMM_R4_PROVIDER_OUT");
    const auto dir = std::filesystem::path(configured ? configured : "runtime_water_checkpoint");
    std::filesystem::create_directories(dir);
    for (const std::string closure : {"CLOSED_FORM", "ENSLAVED", "SIGMA"}) {
        SCOPED_TRACE(closure);
        std::vector<Engine> engines;
        for (int k = 0; k < 2; ++k) {
            const auto base = dir / (closure + "-" + std::to_string(k));
            const auto inp = base.string() + ".inp";
            std::ofstream(inp) << R"([OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
END_DATE 01/01/2026
END_TIME 00:01:00
ROUTING_STEP 3
REPORT_STEP 00:00:06
[JUNCTIONS]
J1 0 3
[2D_OPTIONS]
INTEGRATOR EXPLICIT
LTS_TIERS 1
MAX_TIMESTEP 3
DRY_DEPTH 0.00000001
REPORT_2D NO
[2D_VERTICES]
0 0 10
10 0 10
10 10 10
0 10 10
[2D_TRIANGLES]
0 1 2 0.03 0.02
0 2 3 0.03 0.02
[2D_AQUIFER_OPTIONS]
CLOSURE )" << closure << R"(
[2D_AQUIFER]
* 36 4 0.45 0.1 2 HG0 1
)";
            engines.emplace_back(new openswmm::SWMMEngine);
            auto& e = engines.back();
            ASSERT_EQ(swmm_engine_open(e.get(), inp.c_str(), (base.string() + ".rpt").c_str(),
                                       (base.string() + ".out").c_str(), nullptr),
                      SWMM_OK);
            ASSERT_EQ(swmm_engine_initialize(e.get()), SWMM_OK);
            ASSERT_EQ(swmm_engine_start(e.get(), 0), SWMM_OK);
            frame(e, "pump",
                  {{sizeof(SWMM_CouplingSource), "in", 0, 0, .01, 0, 9.25, 0, nullptr, nullptr},
                   {sizeof(SWMM_CouplingSource), "out", 0, 1, -2, 0, 12.875, 0, nullptr, nullptr}},
                  {});
            frame(e, "well",
                  {{sizeof(SWMM_CouplingSource), "in", 1, 0, .01, 0, 12.875, 0, nullptr, nullptr},
                   {sizeof(SWMM_CouplingSource), "out", 1, 1, -2, 0, 12.875, 0, nullptr, nullptr}},
                  {});
            frame(e, "weather", {},
                  {{sizeof(SWMM_CouplingForcing), 1, -1, 1, 1e-6, 11.625, 0, nullptr},
                   {sizeof(SWMM_CouplingForcing), 1, 0, 1, 2e-6, 11.625, 0, nullptr},
                   {sizeof(SWMM_CouplingForcing), 2, -1, 2, 1e-7, 0, 0, nullptr},
                   {sizeof(SWMM_CouplingForcing), 3, 0, 1, 1e-6, 10.5, 0, nullptr}});
            advance(e, 5.375);
        }
        auto& a = engines[0];
        auto& b = engines[1];
        RuntimeWaterCheckpointBinding bind{
            "identical model and replay driver " + closure, {true, true}, 5.375};
        const auto saved = capture(live(a), bind);
        auto& r = b->surfaceRouter2D();
        // Both engines already have identical physical stores. Only the copied
        // API records are restored here; this is not a whole-engine restart.
        r.runtimeSources() = {};
        r.subsurface().runtimeSources() = {};
        r.state().runtime_forcings = {};
        ASSERT_EQ(RuntimeWaterCheckpoint::restore(bind, saved, r.runtimeSources(),
                                                  r.subsurface().runtimeSources(),
                                                  r.state().runtime_forcings),
                  "");
        EXPECT_EQ(capture(live(b), bind), saved);
        for (double t : {7.125, 9.25, 10.5, 11.625, 12.875, 16.375, 20.125}) {
            SCOPED_TRACE(t);
            advance(a, t);
            advance(b, t);
            bind.completed_time = t;
            EXPECT_EQ(capture(live(a), bind), capture(live(b), bind));
            const auto& x = a->surfaceRouter2D();
            const auto& y = b->surfaceRouter2D();
            EXPECT_EQ(x.state().volume, y.state().volume);
            EXPECT_EQ(x.state().depth, y.state().depth);
            EXPECT_EQ(x.state().face_vx, y.state().face_vx);
            EXPECT_EQ(x.state().edge_flux, y.state().edge_flux);
            EXPECT_EQ(x.subsurface().state().hg, y.subsurface().state().hg);
            EXPECT_EQ(x.subsurface().state().hu, y.subsurface().state().hu);
            EXPECT_EQ(x.subsurface().state().led_source_in, y.subsurface().state().led_source_in);
            EXPECT_EQ(x.subsurface().state().led_source_out, y.subsurface().state().led_source_out);
        }
        const auto final = live(a);
        EXPECT_FALSE(final.surface.find("provider/pump/in", 0)->enabled);
        EXPECT_GT(final.surface.find("provider/pump/out", 1)->receipt.water_out, 0);
        EXPECT_GT(final.groundwater.find("provider/well/out", 1)->receipt.water_out, 0);
        EXPECT_GT(final.forcing.rows[3].receipt.applied, 0);
        EXPECT_TRUE(a->context().runtime_coupling_used);
        EXPECT_TRUE(b->context().runtime_coupling_used);
    }
}

TEST(RuntimeWaterCheckpoint, BoundaryProfileRoundTripTruncationBindingsAndTargetsAreAtomic) {
    BoundaryData authored, live, target;
    authored.resize(12);
    live = target = authored;
    auto b = binding();
    b.boundary_slots.assign(12, false);
    b.boundary_slots[1] = b.boundary_slots[4] = true;
    b.authored_boundary = &authored;
    live.saveRuntime(1);
    live.runtime_owner[1] = "edge";
    live.ensureReceipt(1, 0);
    live.edge_bc_type[1] = int8_t(BoundaryType::SPECIFIED_STAGE);
    live.edge_bc_head[1] = .75;
    live.receipts[1] = {.03, .02, .02, 0, 0, 0, .01, 2, {}, {}};
    live.edge_bc_cum_flux[1] = -.02;
    Stores s, restored;
    auto gw = source("edge", "unused", 1, -.01);
    gw.id = "boundary/1/0";
    gw.boundary_edge = 0;
    gw.receipt = {-.02, -.02, 0, .02, 0, 0, -.01, 2, {}, {}};
    s.groundwater.replace(gw);
    s.groundwater.rebuild(3);
    std::string good;
    ASSERT_EQ(RuntimeWaterCheckpoint::capture(b, s.surface, s.groundwater, s.forcing, good, &live),
              "");
    ASSERT_EQ(RuntimeWaterCheckpoint::restore(b, good, restored.surface, restored.groundwater,
                                              restored.forcing, &target),
              "");
    const auto captured = [&]() {
        std::string value;
        EXPECT_EQ(RuntimeWaterCheckpoint::capture(b, restored.surface, restored.groundwater,
                                                  restored.forcing, value, &target),
                  "");
        return value;
    };
    EXPECT_EQ(captured(), good);
    EXPECT_EQ(target.runtime_owner.at(1), "edge");
    EXPECT_EQ(target.runtime_original.at(1).type, int8_t(BoundaryType::WALL));
    for (std::size_t size = 0; size < good.size(); ++size) {
        SCOPED_TRACE(size);
        EXPECT_NE(RuntimeWaterCheckpoint::restore(b, good.substr(0, size), restored.surface,
                                                  restored.groundwater, restored.forcing, &target),
                  "");
        EXPECT_EQ(captured(), good);
    }
    for (int change = 0; change < 5; ++change) {
        auto changed = b;
        auto base = authored;
        changed.authored_boundary = &base;
        if (change == 0)
            changed.boundary_slots[1] = false;
        if (change == 1)
            base.edge_bc_head[1] = 1;
        if (change == 2)
            changed.groundwater_cells[1] = false;
        if (change == 3)
            changed.completed_time += .1;
        if (change == 4)
            changed.boundary_slots.pop_back();
        EXPECT_NE(RuntimeWaterCheckpoint::restore(changed, good, restored.surface,
                                                  restored.groundwater, restored.forcing, &target),
                  "");
        EXPECT_EQ(captured(), good);
    }
    EXPECT_NE(RuntimeWaterCheckpoint::restore(b, good, restored.surface, restored.groundwater,
                                              restored.forcing),
              "");
    EXPECT_EQ(captured(), good);
    auto overflow = restored;
    overflow.groundwater.rows[0].receipt.water_in = 1e308;
    overflow.groundwater.rows[0].receipt.applied = 1e308;
    auto extra = source("other", "in", 1, 0);
    extra.receipt.water_in = extra.receipt.applied = 1e308;
    overflow.groundwater.replace(extra);
    overflow.groundwater.rebuild(3);
    std::string sentinel = "unchanged";
    EXPECT_NE(RuntimeWaterCheckpoint::capture(b, overflow.surface, overflow.groundwater,
                                              overflow.forcing, sentinel, &target),
              "");
    EXPECT_EQ(sentinel, "unchanged");
    target.clearRuntime(1);
    target.edge_bc_cum_flux[1] = 0;
    target.receipts.clear();
    std::string before;
    ASSERT_EQ(RuntimeWaterCheckpoint::capture(b, restored.surface, restored.groundwater,
                                              restored.forcing, before, &target),
              "");
    auto unsupported = b;
    auto nonwall = authored;
    nonwall.edge_bc_type[1] = int8_t(BoundaryType::RATING_CURVE);
    unsupported.authored_boundary = &nonwall;
    std::string unchanged = "unchanged";
    EXPECT_NE(RuntimeWaterCheckpoint::capture(unsupported, restored.surface, restored.groundwater,
                                              restored.forcing, unchanged, &target),
              "");
    EXPECT_EQ(unchanged, "unchanged");
    ASSERT_EQ(RuntimeWaterCheckpoint::restore(b, good, restored.surface, restored.groundwater,
                                              restored.forcing, &target),
              "");
    EXPECT_EQ(captured(), good);
}

TEST(RuntimeWaterCheckpoint, AuthoredBoundarySignalsBindRecordsAndValidateFallbackAtomically) {
    openswmm::TableData tables;
    for (const auto& item : std::vector<std::pair<const char*, openswmm::TableType>>{
             {"Stage", openswmm::TableType::TIMESERIES},
             {"Flow", openswmm::TableType::TIMESERIES},
             {"Rating", openswmm::TableType::CURVE_RATING}}) {
        openswmm::Table t;
        t.id = item.first;
        t.type = item.second;
        t.x = {0, 1, 2};
        t.y = {0, .5, 1};
        tables.tables.push_back(t);
    }
    tables.rebuild_index();
    BoundaryData authored;
    authored.resize(12);
    authored.edge_bc_type[1] = int8_t(BoundaryType::NORMAL_FLOW);
    authored.edge_bed_slope[1] = .03;
    authored.edge_bc_type[4] = int8_t(BoundaryType::SPECIFIED_STAGE);
    authored.edge_bc_head[4] = .5;
    authored.edge_bc_type[5] = int8_t(BoundaryType::SPECIFIED_FLOW);
    authored.edge_bc_flow[5] = .01;
    authored.edge_bc_type[8] = int8_t(BoundaryType::SPECIFIED_STAGE);
    authored.edge_bc_tseries[8] = -2;
    authored.edge_bc_tseries_name[8] = "Stage";
    authored.edge_bc_type[9] = int8_t(BoundaryType::SPECIFIED_FLOW);
    authored.edge_bc_flow_tseries[9] = -2;
    authored.edge_bc_flow_tseries_name[9] = "Flow";
    authored.edge_bc_type[10] = int8_t(BoundaryType::RATING_CURVE);
    authored.edge_bc_rating_curve[10] = -2;
    authored.edge_bc_rating_curve_name[10] = "Rating";
    auto b = binding();
    b.boundary_slots.assign(12, false);
    for (int k : {1, 4, 5, 8, 9, 10})
        b.boundary_slots[k] = true;
    b.authored_boundary = &authored;
    b.boundary_tables = &tables;
    auto live = authored, target = authored;
    live.edge_bc_tseries[8] = 0;
    live.edge_bc_head[8] = .7;
    live.edge_bc_flow_tseries[9] = 1;
    live.edge_bc_flow[9] = .2;
    live.edge_bc_rating_curve[10] = 2;
    live.edge_bc_flow[10] = .3;
    live.saveRuntime(8);
    live.runtime_owner[8] = "edge";
    live.ensureReceipt(8, 0);
    live.edge_bc_tseries[8] = -1;
    live.edge_bc_tseries_name[8].clear();
    live.edge_bc_head[8] = .8;
    Stores s, c;
    std::string good;
    ASSERT_EQ(RuntimeWaterCheckpoint::capture(b, s.surface, s.groundwater, s.forcing, good, &live),
              "");
    ASSERT_EQ(
        RuntimeWaterCheckpoint::restore(b, good, c.surface, c.groundwater, c.forcing, &target), "");
    auto captureBoundary = [&] {
        std::string result;
        EXPECT_EQ(RuntimeWaterCheckpoint::capture(b, c.surface, c.groundwater, c.forcing, result,
                                                  &target),
                  "");
        return result;
    };
    EXPECT_EQ(captureBoundary(), good);
    for (std::size_t size = 0; size < good.size(); ++size) {
        SCOPED_TRACE(size);
        EXPECT_NE(RuntimeWaterCheckpoint::restore(b, good.substr(0, size), c.surface, c.groundwater,
                                                  c.forcing, &target),
                  "");
        EXPECT_EQ(captureBoundary(), good);
    }
    for (int mutation = 0; mutation < 8; ++mutation) {
        SCOPED_TRACE(mutation);
        auto changed = tables;
        auto binding = b;
        binding.boundary_tables = &changed;
        if (mutation == 0)
            changed.tables[0].y[1] += .1;
        if (mutation == 1)
            changed.tables[0].type = openswmm::TableType::CURVE_RATING;
        if (mutation == 2)
            changed.tables[0].is_file_based = true;
        if (mutation == 3)
            changed.tables[0].num_cols = 2;
        if (mutation == 4)
            changed.tables[0].x[1] = std::numeric_limits<double>::quiet_NaN();
        if (mutation == 5)
            changed.tables[0].x[1] = 0;
        if (mutation == 6)
            changed.tables[0].id = "Other";
        if (mutation == 7)
            binding.boundary_tables = nullptr;
        changed.rebuild_index();
        EXPECT_NE(RuntimeWaterCheckpoint::restore(binding, good, c.surface, c.groundwater,
                                                  c.forcing, &target),
                  "");
        EXPECT_EQ(captureBoundary(), good);
    }
    for (int mutation = 0; mutation < 6; ++mutation) {
        SCOPED_TRACE(mutation);
        auto changed = target;
        if (mutation == 0)
            changed.runtime_original.at(8).stage_name = "Flow";
        if (mutation == 1)
            changed.runtime_original.at(8).stage_ts = 1;
        if (mutation == 2)
            changed.edge_bc_flow_tseries[9] = 0;
        if (mutation == 3)
            changed.edge_bc_rating_curve[10] = 0;
        if (mutation == 4)
            changed.edge_bed_slope[1] += .01;
        if (mutation == 5)
            changed.edge_bc_head[4] += .1;
        std::string result = "unchanged";
        EXPECT_NE(RuntimeWaterCheckpoint::capture(b, c.surface, c.groundwater, c.forcing, result,
                                                  &changed),
                  "");
        EXPECT_EQ(result, "unchanged");
        EXPECT_EQ(captureBoundary(), good);
    }
    target.clearRuntime(8);
    EXPECT_EQ(target.edge_bc_tseries[8], 0);
    EXPECT_EQ(target.edge_bc_tseries_name[8], "Stage");
    std::string cleared;
    ASSERT_EQ(
        RuntimeWaterCheckpoint::capture(b, c.surface, c.groundwater, c.forcing, cleared, &target),
        "");
    // Mutable cursor hints do not change the immutable signal model.
    tables.tables[0].cursor.index = 1;
    tables.tables[0].cursor.direction = -1;
    ASSERT_EQ(
        RuntimeWaterCheckpoint::restore(b, cleared, c.surface, c.groundwater, c.forcing, &target),
        "");
}
