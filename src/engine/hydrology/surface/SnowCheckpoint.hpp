// SPDX-License-Identifier: Apache-2.0

/**
 * @file      SnowCheckpoint.hpp
 * @brief     Checkpoint persistence for snow model state.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <string>
#include <array>
namespace openswmm {
struct SimulationContext;
namespace snow {
struct SnowSoA;
}
namespace detail {
class CompletedSourceArchive;
}
namespace surface {
class MeshSnow;
/** @brief Encode native snow and, when active, the paired mesh snow owner.
 * @param context Initialized model/forcing identity and compatible native runtime;
 * any nonnegative subcatchment pack assignment triggers native serialization even
 * when global snow processing is ignored. Borrowed without mutation.
 * @param snow Compatible initialized degree-day/native report arrays.
 * @param mesh Optional borrowed mesh owner; null/inactive adds no mesh wrapper.
 * @return Native version-two bytes (possibly empty without native assignments), or
 * combined-version-one wrapper containing native and active mesh archives. Native
 * binary representation is implementation-specific. No owner mutation; allocation,
 * archive or inventory validation exceptions propagate. Caller serializes access and
 * pairs this snapshot with all surrounding water/thermal/source owners. */
std::string saveCombinedSnowCheckpoint(const SimulationContext& context, const snow::SnowSoA& snow,
                                       const MeshSnow* mesh);
/** @brief Validate native and mesh snow together before ordinary publication.
 * @param[in,out] context Matching initialized model/forcing and correctly sized
 * report/runtime arrays; apply can replace runtime, reports and append a warning.
 * @param[in,out] snow Compatible current native geometry/parameter arrays; dynamic
 * inventory is replaced only when apply is true.
 * @param[in,out] mesh Optional initialized owner. Nonempty mesh bytes require an
 * active compatible owner; null uses a private inactive validator.
 * @param bytes Combined-version-one, standalone native version-two, or empty archive.
 * An absent mesh part clears/seeds configured mesh cells, retaining assignments.
 * @param apply False fully validates private candidates without publication/warnings;
 * true restores native first, then publishes the mesh candidate.
 * @param seed True keeps accepted inventory but discards held predictions, clears
 * clocks/cumulative fluxes and rebases initial inventories for a new hot start.
 * @return Empty on success, otherwise compatibility/archive/standard-exception
 * message. Nonempty native decode and mesh validation precede publication. Empty
 * native clearing can partly change live arrays if allocation fails; warning append
 * can fail after both owners were published. Thus an error is not a universal
 * exception rollback guarantee. A seeded old hot start lacking mesh snow emits a
 * warning and starts assigned mesh cells bare. Caller coordinates all other owners. */
std::string restoreCombinedSnowCheckpoint(SimulationContext& context, snow::SnowSoA& snow,
                                          MeshSnow* mesh, const std::string& bytes, bool apply,
                                          bool seed);
namespace ueb {
struct HeldState;
}
/** @brief Encode the native snow restart compatibility identity.
 * @param context Initialized named subcatchments/packs, geometry, snow configuration
 * and resolved weather/series values; borrowed without advancing lookup cursors.
 * @param snow Compatible native fraction/external-owner/ADC and degree-day parameters.
 * @return Native binary bytes binding forcing, names/order, pack assignments, areas,
 * pack/removal data, formulation/options/site, surface fractions, per-pack settings,
 * SI/plowing modes, depletion curves and thermal coefficients. Not display text,
 * cryptographic authentication or a portable schema; allocation/archive errors throw. */
std::string snowModelIdentity(const SimulationContext& context, const snow::SnowSoA& snow);
/** @brief Encode configuration and actual weather values controlling held snow predictions.
 * @param context Initialized snow/heat/weather configuration and resolved tables;
 * valid table indices or missing-index sentinels are required.
 * @return Native binary compatibility bytes including UEB options/site/BC ranges,
 * radiation/cloud/solar settings, native weather options/monthly adjustments, daily
 * and hourly records and values of referenced series. Binds values, not paths alone;
 * does not advance weather/table cursors. Runtime override mode/value has a separate
 * key from snowRuntimeForcing(). Copy/allocation/archive exceptions propagate. */
std::string snowForcingIdentity(const SimulationContext& context);
/** @brief Read the small runtime override key used to invalidate held predictions.
 * @param context Borrowed forcing state; no mutation.
 * @return Temperature mode/value then wind mode/value as four doubles. NONE forces
 * its value slot to zero. Values retain native override units (Fahrenheit, mph);
 * persistence, rain, other weather and full model identity are not encoded here. */
std::array<double, 4> snowRuntimeForcing(const SimulationContext& context);
/** @brief Test whether temperature or wind has a transient active runtime override.
 * @param context Borrowed forcing state.
 * @return True when either corresponding mode is not NONE and persistence is RESET;
 * false otherwise. Does not inspect rain or consume/reset any override. */
bool snowTransientForcing(const SimulationContext& context);
/** @brief Stream and validate accepted/predicted lumped UEB history and consumption clocks.
 * @param[in,out] archive Matching native reader/writer; partial consumption/emission
 * can precede exceptions.
 * @param[in,out] held Compatible initialized state; reading replaces accepted/begin/
 * end inventory, flux, 24-hour thermal memories and duration/elapsed seconds.
 * Validates finite fields, nonnegative inventory, cover/point consistency, flux
 * bounds, clocks and pending accepted-water interpolation within 1e-10 m. Does not
 * check every physical/energy interpolation relation or containing model identity.
 * Throws on invalid fields/archive/allocation; validate into a copy for rollback. */
void archiveHeldSnow(detail::CompletedSourceArchive& archive, ueb::HeldState& held);
/** @brief Stream fixed-layout native degree-day dynamic state and shared report histories.
 * @param[in,out] archive Native reader/writer; fixed vector extents must already match.
 * @param[in,out] snow Compatible initialized arrays; geometry/material/assignment
 * parameters are not streamed. Dynamic depths retain native feet, age clocks native
 * units and UEB transfer energy its stored units; no unit conversion occurs.
 * Validates finite scalars/arrays and selected nonnegative water/age/flux, removed/
 * precipitation age and cover bounds. Errors can leave partial archive/array changes;
 * this helper alone neither validates full model identity nor creates a restart. */
void archiveDegreeDaySnow(detail::CompletedSourceArchive& archive, snow::SnowSoA& snow);
/** @brief Encode native degree-day and UEB accepted/held snow state without owner mutation.
 * @param context Compatible initialized model/runtime, resolved forcing and report layout.
 * @param snow Compatible initialized native geometry, parameters and dynamic arrays.
 * @return Native version-two archive binding exact model/forcing identity and including
 * unconsumed UEB predictions, 24-hour temperature memories, clocks and ledgers. Copies
 * state and validates streamed dynamic fields; malformed inventory, allocation and
 * archive errors throw. Caller serializes access; bytes are not portable interchange. */
std::string saveSnowCheckpoint(const SimulationContext& context, const snow::SnowSoA& snow);
/** @brief Decode native snow into a compatible model, optionally as a new hot start.
 * @param[in,out] context Matching initialized snow/forcing identity and runtime/report
 * array extents. Publication changes runtime and current snow depths in native feet;
 * seed also updates old depths. Report depth includes solid plus free water.
 * @param[in,out] snow Initialized geometry/parameter/assignment arrays; nonempty archive
 * is decoded into a private copy before dynamic-state replacement.
 * @param bytes Exact native version-two bytes or empty for bare snow initialization.
 * Nonempty requires full archive consumption, identity/layout/finite/clock checks and
 * agreement between external report and accepted UEB water within 1e-10 m.
 * @param apply False performs nonempty decode/seed validation without publication;
 * empty bytes simply succeed. True installs state, or clears live snow for empty bytes.
 * @param seed True retains accepted water/thermal inventory and histories, closes future
 * predictions, resets clocks/flux/cumulative ledgers and rebases whole non-LID UEB m3/kJ
 * initial inventory. Runtime override key and precipitation-age tracking remain.
 * @return Empty on success or caught standard-exception diagnostic. Nonempty decode
 * protects live owners before publication; empty clearing mutates arrays before fresh
 * runtime allocations, so allocation failure can leave a cleared prefix. Requires
 * correct surrounding context extents; does not rebuild geometry/forcing or qualify
 * other source, mesh, hydraulic and thermal checkpoint owners. Serialize mutations. */
std::string restoreSnowCheckpoint(SimulationContext& context, snow::SnowSoA& snow,
                                  const std::string& bytes, bool apply = true, bool seed = false);
} // namespace surface
} // namespace openswmm
