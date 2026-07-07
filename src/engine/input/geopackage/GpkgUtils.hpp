/**
 * @file GpkgUtils.hpp
 * @brief RAII wrappers and utilities for SQLite operations in GeoPackage I/O.
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GPKG_UTILS_HPP
#define OPENSWMM_GPKG_UTILS_HPP

#include <sqlite3.h>
#include <string>
#include <stdexcept>
#include <memory>
#include <vector>

namespace openswmm::gpkg {

// ============================================================================
// Exception
// ============================================================================

class GpkgError : public std::runtime_error {
public:
    explicit GpkgError(const std::string& msg) : std::runtime_error(msg) {}
    GpkgError(const std::string& msg, int rc)
        : std::runtime_error(msg + " (sqlite rc=" + std::to_string(rc) + ")") {}
};

// ============================================================================
// RAII: Database handle
// ============================================================================

struct DbDeleter {
    void operator()(sqlite3* db) const noexcept {
        if (db) sqlite3_close_v2(db);
    }
};
using DbPtr = std::unique_ptr<sqlite3, DbDeleter>;

inline DbPtr open_database(const std::string& path, int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) {
    sqlite3* raw = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
    DbPtr db(raw);
    if (rc != SQLITE_OK) {
        std::string msg = raw ? sqlite3_errmsg(raw) : "unknown error";
        throw GpkgError("Failed to open database '" + path + "': " + msg, rc);
    }

    // Slice IO-5: every connection enforces foreign keys. This is a
    // per-connection SQLite setting, so we set it here rather than only
    // in create_schema — reader connections need it too if they want
    // cascading-delete or orphan-rejection semantics to hold.
    char* err = nullptr;
    if (sqlite3_exec(db.get(), "PRAGMA foreign_keys=ON", nullptr, nullptr,
                      &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw GpkgError("Failed to enable foreign_keys pragma on '" + path
                          + "': " + msg, rc);
    }

    // Retry on a busy database rather than failing immediately with
    // SQLITE_BUSY. On Windows (mandatory file locking) a connection that is
    // torn down while a write is still settling can briefly hold the .gpkg /
    // -wal lock; without a busy handler the next open()'s first PRAGMA fails
    // outright ("database is locked"). 5 s is generous for the short-lived
    // intra-process contention we actually hit and is a no-op on POSIX, where
    // advisory locking already tolerates this.
    sqlite3_busy_timeout(db.get(), 5000);

    return db;
}

// ============================================================================
// RAII: Prepared statement
// ============================================================================

struct StmtDeleter {
    void operator()(sqlite3_stmt* stmt) const noexcept {
        if (stmt) sqlite3_finalize(stmt);
    }
};
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

inline StmtPtr prepare(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()), &raw, nullptr);
    StmtPtr stmt(raw);
    if (rc != SQLITE_OK) {
        throw GpkgError("Failed to prepare: " + sql + " — " + sqlite3_errmsg(db), rc);
    }
    return stmt;
}

// ============================================================================
// Helpers
// ============================================================================

inline void exec(sqlite3* db, const std::string& sql) {
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        throw GpkgError("exec failed: " + msg + " — SQL: " + sql, rc);
    }
}

inline void bind_text(sqlite3_stmt* stmt, int col, const std::string& val) {
    sqlite3_bind_text(stmt, col, val.c_str(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
}

inline void bind_double(sqlite3_stmt* stmt, int col, double val) {
    sqlite3_bind_double(stmt, col, val);
}

inline void bind_int(sqlite3_stmt* stmt, int col, int val) {
    sqlite3_bind_int(stmt, col, val);
}

inline void bind_null(sqlite3_stmt* stmt, int col) {
    sqlite3_bind_null(stmt, col);
}

inline void bind_blob(sqlite3_stmt* stmt, int col, const void* data, int size) {
    sqlite3_bind_blob(stmt, col, data, size, SQLITE_TRANSIENT);
}

inline std::string column_text(sqlite3_stmt* stmt, int col) {
    const unsigned char* txt = sqlite3_column_text(stmt, col);
    return txt ? std::string(reinterpret_cast<const char*>(txt)) : std::string{};
}

inline double column_double(sqlite3_stmt* stmt, int col) {
    return sqlite3_column_double(stmt, col);
}

inline int column_int(sqlite3_stmt* stmt, int col) {
    return sqlite3_column_int(stmt, col);
}

inline bool column_is_null(sqlite3_stmt* stmt, int col) {
    return sqlite3_column_type(stmt, col) == SQLITE_NULL;
}

inline std::vector<uint8_t> column_blob(sqlite3_stmt* stmt, int col) {
    int size = sqlite3_column_bytes(stmt, col);
    const uint8_t* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, col));
    if (!data || size <= 0) return {};
    return {data, data + size};
}

// RAII transaction guard.
//
// The destructor rolls a not-yet-committed transaction back, but does so
// ROBUSTLY instead of fire-and-forget. A plain, unchecked
// `sqlite3_exec(db, "ROLLBACK")` can silently leave the transaction OPEN on
// Windows: if any statement on the connection is still in an aborted/stepped
// state — e.g. an INSERT that just tripped an immediate FOREIGN KEY constraint
// mid-write — SQLite refuses the ROLLBACK with SQLITE_BUSY ("cannot rollback -
// SQL statements in progress"). Under Windows' mandatory file locking the
// transaction then stays open and the SAME connection reads its own uncommitted
// rows; POSIX advisory locking tolerates the stray lock, which is why the bug
// only surfaced on Windows (test_engine_geopackage_mesh2d rollback case, where
// the connection saw 3 nodes / 4 vertices / 60 options instead of none).
//
// Resetting only the one statement that failed (in step_or_throw) was not
// enough — anything still holding a per-statement lock at unwind time blocks the
// ROLLBACK. rollback() therefore drives the connection to a known-clean state:
//   1. reset EVERY live statement so none holds a statement-journal lock,
//   2. issue ROLLBACK,
//   3. confirm the connection actually returned to autocommit mode,
// retrying a bounded number of times. On POSIX this is a single clean pass and
// the reset loop is a no-op (unwinding has already finalized the statements).
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) {
        exec(db_, "BEGIN IMMEDIATE");
    }
    void commit() {
        if (!finished_) {
            exec(db_, "COMMIT");
            finished_ = true;
        }
    }
    void rollback() {
        if (finished_) return;
        for (int attempt = 0; attempt < 3; ++attempt) {
            // Clear any statement left mid-flight so it cannot hold a lock that
            // makes ROLLBACK fail with SQLITE_BUSY (statements in progress).
            for (sqlite3_stmt* s = sqlite3_next_stmt(db_, nullptr); s != nullptr;
                 s = sqlite3_next_stmt(db_, s)) {
                sqlite3_reset(s);
            }
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            // sqlite3_get_autocommit() != 0 iff no transaction is open, i.e. the
            // ROLLBACK actually took effect. If it is still 0 the rollback was
            // refused; loop and retry now that the statements are reset.
            if (sqlite3_get_autocommit(db_) != 0) break;
        }
        finished_ = true;
    }
    ~Transaction() {
        rollback();
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
private:
    sqlite3* db_;
    bool finished_ = false;
};

} // namespace openswmm::gpkg

#endif // OPENSWMM_GPKG_UTILS_HPP
