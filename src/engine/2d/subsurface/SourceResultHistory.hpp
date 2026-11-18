// SPDX-License-Identifier: Apache-2.0

/**
 * @file      SourceResultHistory.hpp
 * @brief     Columnar source diagnostics and saved identifier tables.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <map>
#include <cstdint>
#include <string>
#include <vector>

namespace openswmm::twoD {
class CommonSourceStage;

/// Columnar diagnostics. Identifiers refer to the saved name/usage tables;
/// Frame meaning belongs to its recorder, never to the hydraulic report index.
struct SourceResultTable {
    std::vector<std::string> fields, units;
    std::vector<std::vector<double>> columns;
    /**
     * @brief Read the first column's row count without validating the remaining columns.
     * @return Zero if no columns exist, otherwise columns.front().size(). A valid table has
     * equal column lengths; callers must validate that invariant separately. No allocation or
     * mutation.
     */
    std::size_t rows() const { return columns.empty() ? 0 : columns.front().size(); }
};

/// Internal R4 qualification recorder: explicitly capture the initial boundary
/// and EVERY accepted source interval. Capture failure changes neither physics
/// nor history. Receiver clocks and pending queues are endpoint snapshots.
/// Optional marcher/writer hooks remain behind the reviewed runtime gate.
/// Paired engine continuation uses EngineSourceResultHistory; production
/// streaming remains unqualified.
class SourceResultHistory {
public:
    /**
     * @brief Capture frame zero or the next accepted source interval with transactional history
     * publication.
     * @par Argument The unnamed stage reference must be a stable prepared checkpoint-capable
     * source stage; after first capture its exact address/model must match. Its dependencies
     * remain borrowed and must live through future recorder use. Capture does not advance
     * physics.
     * @return Empty on validated publication, otherwise a diagnostic with history unchanged.
     * Requires initial interval/end zero and then each strictly increasing accepted interval
     * exactly once. Stores cumulative SI water/native pollutant receipts and an exact component
     * checkpoint anchor.
     */
    std::string append(const CommonSourceStage&);
    /**
     * @brief Rebind only an exact component-checkpoint match and verified final diagnostic
     * projection. Does not restore any hydraulic engine state.
     *
     * @par Argument and result The unnamed stage reference is a stable restored
     * checkpoint-capable stage. Returns empty only after exact checkpoint bytes, capability,
     * final clocks, identities, ownership and reprojected numeric diagnostics match. Failure
     * preserves this history and all physics. Success binds a borrowed live stage pointer for
     * later append(); this does not install any component or engine state.
     */
    std::string resume(const CommonSourceStage&);
    /**
     * @brief Borrow this history's columnar tables and their field/unit schemas.
     * @return Const reference owned by this recorder; valid until its destruction or
     * replacement. append(), resume(), read() publication or assignment may invalidate
     * elements/references. No copy, allocation or synchronization occurs; keep the recorder
     * stable while accessing the view.
     */
    const std::map<std::string, SourceResultTable>& tables() const { return tables_; }
    /**
     * @brief Borrow this history's saved source names indexed by source identifiers.
     * @return Const reference owned by this recorder; valid until its destruction or
     * replacement. append(), resume(), read() publication or assignment may invalidate
     * elements/references. No copy, allocation or synchronization occurs; keep the recorder
     * stable while accessing the view.
     */
    const std::vector<std::string>& sourceNames() const { return sources_; }
    /**
     * @brief Borrow this history's saved pollutant names indexed by species identifiers.
     * @return Const reference owned by this recorder; valid until its destruction or
     * replacement. append(), resume(), read() publication or assignment may invalidate
     * elements/references. No copy, allocation or synchronization occurs; keep the recorder
     * stable while accessing the view.
     */
    const std::vector<std::string>& pollutantNames() const { return pollutants_; }
    /**
     * @brief Borrow this history's saved per-pollutant native_concentration*m3 unit labels.
     * @return Const reference owned by this recorder; valid until its destruction or
     * replacement. append(), resume(), read() publication or assignment may invalidate
     * elements/references. No copy, allocation or synchronization occurs; keep the recorder
     * stable while accessing the view.
     */
    const std::vector<std::string>& pollutantMassUnits() const { return mass_units_; }
    /**
     * @brief Optional water-only hydraulic-return diagnostics. Older histories and profile-off
     * runs have no such capability; absence is not a zero series.
     *
     * @details Test the saved water-only hydraulic-return capability.
     * @return True when optional return columns/clocks are part of this history's schema. False
     * means unavailable, including old/profile-off histories; it must not be interpreted as a
     * zero series.
     */
    bool outfallReturnsAvailable() const { return outfall_returns_; }
    /**
     * @brief Standalone HDF5 result artifact; atomically replaces ONLY this artifact. Never pass
     * an existing hydraulic results/model file as the destination.
     *
     * @param[in] path Standalone destination. An existing file must identify itself as a source
     * history artifact; hydraulic/model files are rejected. Parent directory must already exist.
     * @return Empty on success, otherwise validation/I/O/unsupported-build diagnostic. Validated
     * data is written to an exclusively created sibling, flushed/closed, then renamed into
     * place. Temporary publication files are removed on caught failure; an exclusive-create
     * collision is never deleted.
     * @note Serializes calls through the engine HDF5 mutex. Same-path writers still require
     * caller coordination. Filesystem rename publication supplies no power-loss/durability
     * guarantee.
     */
    std::string save(const std::string& path) const;
    /**
     * @brief An older HDF5 file without /source_history is successfully unavailable.
     * Failure/unavailability preserve out. Append requires explicit verified resume.
     *
     * @param[in] path Existing nonempty HDF5 file, at most 256 MiB. Missing source_history is a
     * successful unavailable result; unfinished publication links are rejected.
     * @param[out] out Replaced only after full schema/metadata/shape/identity/receipt
     * validation; remains unchanged on failure or unavailable capability. Returned history needs
     * explicit resume() to append.
     * @param[out] available Set false immediately; true only when a validated history replaces
     * out.
     * @return Empty on success (including unavailable group), otherwise
     * validation/I/O/unsupported-build diagnostic. Older accepted source schemas are supported
     * subject to exact metadata checks.
     * @note Uses the engine HDF5 mutex; out must remain exclusively accessible during
     * publication.
     */
    static std::string read(const std::string& path, SourceResultHistory& out, bool& available);

private:
    friend struct SourceResultFile;
    friend struct EngineSourceResultFile;
    friend class EngineSourceResultHistory;
    friend class EngineSourceResultStream;
    void appendRows(const CommonSourceStage&, std::size_t frame, double start);
    static SourceResultHistory projection(const CommonSourceStage&, std::size_t frame,
                                          double start);
    friend class Default2DOutputPlugin;
    friend class ExplicitInertialSolver;
    std::string refreshCheckpointMatch(const CommonSourceStage&);
    void initializeIdentity(const CommonSourceStage&);
    std::string resumeCapturedBoundary(const CommonSourceStage&);
    /**
     * @brief Publish source_history within an engine-owned open HDF5 result instance.
     * @param[in] file Borrowed valid open HDF5 file ID from the engine linked library; no
     * ownership transfer.
     * @par Argument and result The unnamed signed 64-bit handle must be a valid open file from
     * the engine's linked HDF5 instance. Returns empty after validated staged-group publication,
     * or a diagnostic. Other root datasets/time axes are untouched. Uses the engine HDF5 mutex;
     * existing unfinished staging links refuse work.
     * @note Staged/previous links permit best-effort rollback on caught process errors, not
     * crash or power-loss atomicity. Rollback HDF5 return codes are unchecked, so a failure can
     * leave the file requiring inspection. Does not close the borrowed file or provide the
     * production streaming gate.
     */
    std::string writeIntoOpenResult(std::int64_t file) const; // engine's HDF5 instance only
    std::string extends(const SourceResultHistory& prefix) const;
    static std::map<std::string, SourceResultTable> schema(bool outfall_returns = false);
    std::string validate() const;
    const CommonSourceStage* stage_ = nullptr;
    bool outfall_returns_ = false;
    std::string model_, boundary_;
    std::vector<std::string> sources_, pollutants_, mass_units_;
    std::map<std::string, SourceResultTable> tables_;
};
}
