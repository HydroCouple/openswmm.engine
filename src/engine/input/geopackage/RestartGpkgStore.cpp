// SPDX-License-Identifier: Apache-2.0
/**
 * @file      RestartGpkgStore.cpp
 * @brief     Restart records in a GeoPackage.
 */
#include "RestartGpkgStore.hpp"

#include "GeoPackageSchema.hpp"
#include "GeoPackageWriter.hpp"
#include "GpkgUtils.hpp"
#include "core/FileIO.hpp"
#include "core/restart/Sha256.hpp"

#include <version.h>

#include <algorithm>
#include <filesystem>
#include <map>

namespace openswmm::gpkg {
namespace {

constexpr std::size_t kChunkBytes = 4u * 1024u * 1024u;

constexpr const char* kRecordDdl = R"SQL(
CREATE TABLE IF NOT EXISTS restart_records (
    simulation_id      TEXT NOT NULL,
    record_id          TEXT NOT NULL,
    ordinal            INTEGER NOT NULL,
    created_at         TEXT NOT NULL,
    model_datetime     REAL NOT NULL,   -- decimal days of the accepted step captured
    elapsed_seconds    REAL NOT NULL,
    requested_datetime REAL NOT NULL,   -- decimal days asked for; 0 for END
    hsf_version        INTEGER NOT NULL,
    continuation       INTEGER NOT NULL,
    binary_output      INTEGER NOT NULL DEFAULT 0, -- the checkpoint carries the .out so far
    byte_count         INTEGER NOT NULL,
    chunk_count        INTEGER NOT NULL,
    sha256             TEXT NOT NULL,
    platform           TEXT NOT NULL,
    engine_version     TEXT NOT NULL,
    status             TEXT NOT NULL,
    PRIMARY KEY (simulation_id, record_id),
    FOREIGN KEY (simulation_id) REFERENCES simulations(simulation_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS restart_record_chunks (
    simulation_id  TEXT NOT NULL,
    record_id      TEXT NOT NULL,
    chunk_index    INTEGER NOT NULL,
    bytes          BLOB NOT NULL,
    PRIMARY KEY (simulation_id, record_id, chunk_index),
    FOREIGN KEY (simulation_id, record_id)
        REFERENCES restart_records(simulation_id, record_id) ON DELETE CASCADE
);
)SQL";

std::string text(sqlite3_stmt* s, int col) {
    return column_is_null(s, col) ? std::string{} : column_text(s, col);
}

class GpkgStore final : public restart::Store {
public:
    GpkgStore(DbPtr db, std::string path) : db_(std::move(db)), path_(std::move(path)) {}

    std::string catalog(restart::Catalog& out) override {
        out = {};
        out.backend = restart::Backend::GEOPACKAGE;
        try {
            if (!hasTable("restart_records"))
                return {};
            std::map<std::string, std::size_t> index;
            auto runs = prepare(db_.get(),
                                "SELECT s.simulation_id, s.created_at, s.engine_version, "
                                "s.parent_run_id, s.parent_record_id, s.restart_mode, "
                                "s.restart_source FROM simulations s WHERE EXISTS (SELECT 1 FROM "
                                "restart_records r WHERE r.simulation_id = s.simulation_id) "
                                "ORDER BY s.created_at, s.simulation_id");
            const bool provenance = hasColumn("simulations", "parent_run_id");
            while (sqlite3_step(runs.get()) == SQLITE_ROW) {
                restart::RunInfo run;
                run.run_id = text(runs.get(), 0);
                run.created_at = text(runs.get(), 1);
                run.engine_version = text(runs.get(), 2);
                if (provenance) {
                    run.parent_run_id = text(runs.get(), 3);
                    run.parent_record_id = text(runs.get(), 4);
                    run.restart_mode = text(runs.get(), 5);
                    run.restart_source = text(runs.get(), 6);
                }
                index[run.run_id] = out.runs.size();
                out.runs.push_back({run, {}});
            }
            const bool has_output = hasColumn("restart_records", "binary_output");
            auto records = prepare(
                db_.get(), std::string("SELECT simulation_id, record_id, ordinal, created_at, "
                                       "model_datetime, elapsed_seconds, requested_datetime, "
                                       "hsf_version, continuation, byte_count, sha256, platform, "
                                       "engine_version, status, ") +
                               (has_output ? "binary_output" : "0") +
                               " FROM restart_records ORDER BY simulation_id, ordinal");
            while (sqlite3_step(records.get()) == SQLITE_ROW) {
                auto it = index.find(text(records.get(), 0));
                if (it == index.end())
                    continue;
                restart::RecordInfo r;
                r.record_id = text(records.get(), 1);
                r.ordinal = column_int(records.get(), 2);
                r.created_at = text(records.get(), 3);
                r.model_datetime = column_double(records.get(), 4);
                r.elapsed_seconds = column_double(records.get(), 5);
                r.requested_datetime = column_double(records.get(), 6);
                r.hsf_version = column_int(records.get(), 7);
                r.continuation = column_int(records.get(), 8) != 0;
                r.byte_count = static_cast<std::uint64_t>(sqlite3_column_int64(records.get(), 9));
                r.sha256 = text(records.get(), 10);
                r.platform = text(records.get(), 11);
                r.engine_version = text(records.get(), 12);
                r.status = text(records.get(), 13);
                r.binary_output = column_int(records.get(), 14) != 0;
                out.runs[it->second].second.push_back(std::move(r));
            }
            return {};
        } catch (const std::exception& e) {
            return std::string("cannot list the restart records of '") + path_ + "': " + e.what();
        }
    }

    std::string read(const std::string& run_id, const std::string& record_id,
                     std::string& bytes) override {
        bytes.clear();
        try {
            if (!hasTable("restart_records"))
                return "'" + path_ + "' holds no restart records";
            auto meta = prepare(db_.get(), "SELECT byte_count, chunk_count, sha256, status FROM "
                                           "restart_records WHERE simulation_id=? AND record_id=?");
            bind_text(meta.get(), 1, run_id);
            bind_text(meta.get(), 2, record_id);
            if (sqlite3_step(meta.get()) != SQLITE_ROW)
                return "'" + path_ + "' has no record " + record_id + " in run " + run_id;
            const auto size = static_cast<std::uint64_t>(sqlite3_column_int64(meta.get(), 0));
            const int chunks = column_int(meta.get(), 1);
            const std::string digest = text(meta.get(), 2);
            if (text(meta.get(), 3) != "complete")
                return "record " + record_id + " of run " + run_id + " is not complete";
            auto data = prepare(db_.get(), "SELECT chunk_index, bytes FROM restart_record_chunks "
                                           "WHERE simulation_id=? AND record_id=? "
                                           "ORDER BY chunk_index");
            bind_text(data.get(), 1, run_id);
            bind_text(data.get(), 2, record_id);
            int expected = 0;
            while (sqlite3_step(data.get()) == SQLITE_ROW) {
                if (column_int(data.get(), 0) != expected++)
                    return "record " + record_id + " is missing a chunk";
                const auto* blob = static_cast<const char*>(sqlite3_column_blob(data.get(), 1));
                bytes.append(blob ? blob : "",
                             static_cast<std::size_t>(sqlite3_column_bytes(data.get(), 1)));
            }
            if (expected != chunks || bytes.size() != size)
                return "record " + record_id + " of run " + run_id + " is truncated";
            if (restart::sha256Hex(bytes) != digest)
                return "record " + record_id + " of run " + run_id +
                       " does not match its SHA-256 digest";
            return {};
        } catch (const std::exception& e) {
            return std::string("cannot read record ") + record_id + ": " + e.what();
        }
    }

    std::string beginRun(const SimulationContext& ctx, const restart::RunInfo& run) override {
        try {
            create_schema(db_.get());
            exec(db_.get(), kRecordDdl);
            auto exists = prepare(db_.get(), "SELECT 1 FROM simulations WHERE simulation_id=?");
            bind_text(exists.get(), 1, run.run_id);
            const bool known = sqlite3_step(exists.get()) == SQLITE_ROW;
            exists.reset();
            // A dedicated record file keeps the model it ran, as a results file does.
            if (!known)
                write_model(db_.get(), ctx, run.run_id);
            auto mark = prepare(
                db_.get(), "UPDATE simulations SET status = CASE status WHEN 'model' THEN "
                           "'running' ELSE status END, parent_run_id = ?, parent_record_id = ?, "
                           "restart_mode = ?, restart_source = ? WHERE simulation_id = ?");
            auto bind_optional = [&](int col, const std::string& v) {
                if (v.empty())
                    bind_null(mark.get(), col);
                else
                    bind_text(mark.get(), col, v);
            };
            bind_optional(1, run.parent_run_id);
            bind_optional(2, run.parent_record_id);
            bind_optional(3, run.restart_mode);
            bind_optional(4, run.restart_source);
            bind_text(mark.get(), 5, run.run_id);
            checked_model_step(db_.get(), mark.get());
            return {};
        } catch (const std::exception& e) {
            return "cannot record restart states into '" + path_ + "': " + e.what();
        }
    }

    std::string append(const std::string& run_id, const restart::RecordInfo& r,
                       const std::string& bytes) override {
        try {
            const std::size_t chunks = (bytes.size() + kChunkBytes - 1) / kChunkBytes;
            Transaction txn(db_.get());
            auto row = prepare(db_.get(),
                               "INSERT INTO restart_records (simulation_id, record_id, ordinal, "
                               "created_at, model_datetime, elapsed_seconds, requested_datetime, "
                               "hsf_version, continuation, byte_count, chunk_count, sha256, "
                               "platform, engine_version, binary_output, status) "
                               "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'writing')");
            bind_text(row.get(), 1, run_id);
            bind_text(row.get(), 2, r.record_id);
            bind_int(row.get(), 3, r.ordinal);
            bind_text(row.get(), 4, r.created_at);
            bind_double(row.get(), 5, r.model_datetime);
            bind_double(row.get(), 6, r.elapsed_seconds);
            bind_double(row.get(), 7, r.requested_datetime);
            bind_int(row.get(), 8, r.hsf_version);
            bind_int(row.get(), 9, r.continuation ? 1 : 0);
            sqlite3_bind_int64(row.get(), 10, static_cast<sqlite3_int64>(bytes.size()));
            bind_int(row.get(), 11, static_cast<int>(chunks));
            bind_text(row.get(), 12, restart::sha256Hex(bytes));
            bind_text(row.get(), 13, r.platform);
            bind_text(row.get(), 14, r.engine_version);
            bind_int(row.get(), 15, r.binary_output ? 1 : 0);
            checked_model_step(db_.get(), row.get());
            auto chunk = prepare(db_.get(), "INSERT INTO restart_record_chunks (simulation_id, "
                                            "record_id, chunk_index, bytes) VALUES (?,?,?,?)");
            for (std::size_t i = 0; i < chunks; ++i) {
                sqlite3_reset(chunk.get());
                bind_text(chunk.get(), 1, run_id);
                bind_text(chunk.get(), 2, r.record_id);
                bind_int(chunk.get(), 3, static_cast<int>(i));
                const std::size_t at = i * kChunkBytes;
                const std::size_t n = std::min(kChunkBytes, bytes.size() - at);
                sqlite3_bind_blob(chunk.get(), 4, bytes.data() + at, static_cast<int>(n),
                                  SQLITE_TRANSIENT);
                checked_model_step(db_.get(), chunk.get());
            }
            // The record becomes complete in the same transaction as its bytes.
            auto done = prepare(db_.get(), "UPDATE restart_records SET status='complete' "
                                           "WHERE simulation_id=? AND record_id=?");
            bind_text(done.get(), 1, run_id);
            bind_text(done.get(), 2, r.record_id);
            checked_model_step(db_.get(), done.get());
            txn.commit();
            return {};
        } catch (const std::exception& e) {
            return "cannot store restart record " + r.record_id + " in '" + path_ +
                   "': " + e.what();
        }
    }

    std::string finishRun(const std::string& run_id) override {
        try {
            auto done =
                prepare(db_.get(), "UPDATE simulations SET status='completed', finished_at = "
                                   "COALESCE(finished_at, strftime('%Y-%m-%dT%H:%M:%SZ','now')) "
                                   "WHERE simulation_id=? AND status='running'");
            bind_text(done.get(), 1, run_id);
            checked_model_step(db_.get(), done.get());
            return {};
        } catch (const std::exception& e) {
            return "cannot finish run " + run_id + " in '" + path_ + "': " + e.what();
        }
    }

private:
    bool hasTable(const char* name) {
        auto s = prepare(db_.get(), "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
        bind_text(s.get(), 1, name);
        return sqlite3_step(s.get()) == SQLITE_ROW;
    }
    bool hasColumn(const char* table, const char* column) {
        auto s = prepare(db_.get(), std::string("PRAGMA table_info(") + table + ")");
        while (sqlite3_step(s.get()) == SQLITE_ROW)
            if (column_text(s.get(), 1) == column)
                return true;
        return false;
    }

    DbPtr db_;
    std::string path_;
};

/** SQLite URI of @p path, with the characters a URI path reserves escaped. */
std::string immutableUri(const std::string& path) {
    std::string p = path;
#ifdef _WIN32
    for (char& c : p)
        if (c == '\\')
            c = '/';
    if (p.size() > 1 && p[1] == ':')
        p.insert(p.begin(), '/');
#endif
    std::string out = "file:";
    for (const char c : p) {
        if (c == '%')
            out += "%25";
        else if (c == '?')
            out += "%3F";
        else if (c == '#')
            out += "%23";
        else
            out += c;
    }
    return out + "?immutable=1";
}

} // namespace

std::unique_ptr<restart::Store> openRestartStore(const std::string& path, bool write,
                                                 std::string& error) {
    try {
        // Reading a WAL database normally creates -wal and -shm files beside it,
        // and fails in a read-only folder. Without a -wal file no writer holds
        // uncommitted content, so the file is read as immutable: nothing is
        // created and nothing is locked.
        std::error_code ec;
        const bool wal = std::filesystem::exists(io::utf8_path(path + "-wal"), ec);
        auto db = write ? open_database(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
                  : wal ? open_database(path, SQLITE_OPEN_READONLY)
                        : open_database(immutableUri(path), SQLITE_OPEN_READONLY | SQLITE_OPEN_URI);
        return std::make_unique<GpkgStore>(std::move(db), path);
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

} // namespace openswmm::gpkg
