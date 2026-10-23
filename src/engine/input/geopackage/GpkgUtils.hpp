// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      GpkgUtils.hpp
 * @brief     RAII wrappers and utilities for SQLite operations in GeoPackage I/O.
 * @ingroup engine_geopackage
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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

/**
 * @brief Runtime-error carrier for owned GeoPackage/SQLite diagnostic text.
 */
class GpkgError : public std::runtime_error {
public:
    /**
     * @brief Own a supplied GeoPackage failure message.
     *
     * Allocation exceptions can propagate during construction.
     *
     * @param[in] msg Borrowed message copied into std::runtime_error.
     */
    explicit GpkgError(const std::string& msg) : std::runtime_error(msg) {}
    /**
     * @brief Own a failure message with a formatted SQLite result code.
     *
     * @param[in] msg Borrowed message copied into exception storage.
     * @param[in] rc SQLite result code appended as decimal text; not stored as a separate typed field.
     */
    GpkgError(const std::string& msg, int rc)
        : std::runtime_error(msg + " (sqlite rc=" + std::to_string(rc) + ")") {}
};

// ============================================================================
// RAII: Database handle
// ============================================================================

/**
 * @brief Unique-pointer deleter using SQLite deferred close semantics.
 */
struct DbDeleter {
    /**
     * @brief Best-effort close a uniquely owned SQLite connection.
     *
     * Calls sqlite3_close_v2 and ignores status; SQLite may defer actual destruction until outstanding
     * statements are finalized.
     *
     * @param[in] db Owned connection, or nullptr for no-op; caller releases ownership after this
     *     invocation.
     */
    void operator()(sqlite3* db) const noexcept {
        if (db)
            sqlite3_close_v2(db);
    }
};
using DbPtr = std::unique_ptr<sqlite3, DbDeleter>;

/**
 * @brief Open an owned connection and configure GeoPackage connection defaults.
 *
 * Enables foreign keys and sets a 5000 ms busy timeout after that pragma. No schema creation or
 * transaction begins here. The timeout setup return status is ignored.
 * @throws GpkgError For
 * open failure or foreign-key pragma failure; partially opened connection is closed by RAII.
 *
 * @param[in] path Borrowed SQLite filename token copied into sqlite3_open_v2; default flags allow
 *     creation.
 * @param[in] flags SQLite open flags, default READWRITE|CREATE; caller selects additional mutex/URI
 *     policy as needed.
 * @return Unique connection handle with close_v2 deleter.
 */
inline DbPtr open_database(const std::string& path,
                           int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) {
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
    if (sqlite3_exec(db.get(), "PRAGMA foreign_keys=ON", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw GpkgError("Failed to enable foreign_keys pragma on '" + path + "': " + msg, rc);
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

/**
 * @brief Unique-pointer deleter that finalizes prepared statements.
 */
struct StmtDeleter {
    /**
     * @brief Finalize a uniquely owned SQLite statement.
     *
     * Ignores sqlite3_finalize status; do not use the statement afterward.
     *
     * @param[in] stmt Owned statement, or nullptr for no-op.
     */
    void operator()(sqlite3_stmt* stmt) const noexcept {
        if (stmt)
            sqlite3_finalize(stmt);
    }
};
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

/**
 * @brief Prepare the first SQL statement without stepping it.
 *
 * Connection remains externally owned; parameter bindings/execution are separate.
 * @throws
 * GpkgError When sqlite3_prepare_v2 returns a failure code.
 *
 * @param[in] db Non-null live SQLite connection borrowed for this call; caller serializes
 *     transaction/statement use.
 * @param[in] sql Borrowed SQL text; byte count must fit int. Remaining SQL tail is ignored because
 *     no tail pointer is requested.
 * @return Unique statement handle, possibly null for empty/comment-only SQL.
 */
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

/**
 * @brief Execute SQL text and discard all returned rows.
 *
 * No implicit transaction or rollback. Earlier statements can have succeeded when a later one
 * fails.
 * @throws GpkgError For any sqlite3_exec failure, including SQLite diagnostic and SQL
 * source.
 *
 * @param[in] db Non-null live SQLite connection borrowed for this call; caller serializes
 *     transaction/statement use.
 * @param[in] sql Borrowed NUL-terminated SQL string; may contain multiple statements and may mutate
 *     schema/data.
 */
inline void exec(sqlite3* db, const std::string& sql) {
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        throw GpkgError("exec failed: " + msg + " — SQL: " + sql, rc);
    }
}

/**
 * @brief Bind a copied SQL text parameter, discarding SQLite status.
 *
 * Binding errors are not thrown or returned by this wrapper.
 *
 * @param[in] stmt Non-null live prepared statement; caller serializes access and ensures required
 *     row/binding state.
 * @param[in] col One-based SQL parameter index; SQLite validates it, but this wrapper discards the
 *     return status.
 * @param[in] val Borrowed text bytes copied by SQLITE_TRANSIENT; length must fit int. Embedded NUL
 *     bytes are included in the bound byte count.
 */
inline void bind_text(sqlite3_stmt* stmt, int col, const std::string& val) {
    sqlite3_bind_text(stmt, col, val.c_str(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
}

/**
 * @brief Bind a floating SQL parameter, discarding SQLite status.
 *
 * @param[in] stmt Non-null live prepared statement; caller serializes access and ensures required
 *     row/binding state.
 * @param[in] col One-based SQL parameter index; SQLite validates it, but this wrapper discards the
 *     return status.
 * @param[in] val Numeric value forwarded without unit/range validation.
 */
inline void bind_double(sqlite3_stmt* stmt, int col, double val) {
    sqlite3_bind_double(stmt, col, val);
}

/**
 * @brief Bind an int SQL parameter, discarding SQLite status.
 *
 * @param[in] stmt Non-null live prepared statement; caller serializes access and ensures required
 *     row/binding state.
 * @param[in] col One-based SQL parameter index; SQLite validates it, but this wrapper discards the
 *     return status.
 * @param[in] val Integer value forwarded verbatim.
 */
inline void bind_int(sqlite3_stmt* stmt, int col, int val) {
    sqlite3_bind_int(stmt, col, val);
}

/**
 * @brief Bind SQL NULL, discarding SQLite status.
 *
 * @param[in] stmt Non-null live prepared statement; caller serializes access and ensures required
 *     row/binding state.
 * @param[in] col One-based SQL parameter index; SQLite validates it, but this wrapper discards the
 *     return status.
 */
inline void bind_null(sqlite3_stmt* stmt, int col) {
    sqlite3_bind_null(stmt, col);
}

/**
 * @brief Bind copied binary bytes, discarding SQLite status.
 *
 * SQLITE_TRANSIENT copies the block for statement ownership. Allocation/bind failure is not
 * reported by this wrapper.
 *
 * @param[in] stmt Non-null live prepared statement; caller serializes access and ensures required
 *     row/binding state.
 * @param[in] col One-based SQL parameter index; SQLite validates it, but this wrapper discards the
 *     return status.
 * @param[in] data Borrowed readable block of size bytes; nullptr requests SQL NULL.
 * @param[in] size Nonnegative byte count required for a non-null block; forwarded without
 *     validation.
 */
inline void bind_blob(sqlite3_stmt* stmt, int col, const void* data, int size) {
    sqlite3_bind_blob(stmt, col, data, size, SQLITE_TRANSIENT);
}

/**
 * @brief Copy current-row text using NUL-terminated SQLite conversion.
 *
 * SQLite numeric-to-text conversion may occur; this wrapper does not separately report conversion
 * allocation errors.
 *
 * @param[in] stmt Non-null live prepared statement; caller serializes access and ensures required
 *     row/binding state.
 * @param[in] col Zero-based result-column index valid for the current row; no wrapper bounds check.
 * @return Owned string, empty for SQL NULL/no text pointer. Embedded NUL bytes truncate the copy
 *     because column_bytes is not used.
 */
inline std::string column_text(sqlite3_stmt* stmt, int col) {
    const unsigned char* txt = sqlite3_column_text(stmt, col);
    return txt ? std::string(reinterpret_cast<const char*>(txt)) : std::string{};
}

/**
 * @brief Read current-row SQLite numeric conversion as double.
 *
 * @param[in] stmt Non-null live prepared statement; caller serializes access and ensures required
 *     row/binding state.
 * @param[in] col Zero-based result-column index valid for the current row; no wrapper bounds check.
 * @return sqlite3_column_double() result, including SQLite NULL/type-conversion behavior; no unit
 *     conversion.
 */
inline double column_double(sqlite3_stmt* stmt, int col) {
    return sqlite3_column_double(stmt, col);
}

/**
 * @brief Read current-row SQLite numeric conversion as int.
 *
 * @param[in] stmt Non-null live prepared statement; caller serializes access and ensures required
 *     row/binding state.
 * @param[in] col Zero-based result-column index valid for the current row; no wrapper bounds check.
 * @return sqlite3_column_int() result; no wrapper range/type validation.
 */
inline int column_int(sqlite3_stmt* stmt, int col) {
    return sqlite3_column_int(stmt, col);
}

/**
 * @brief Test the current-row SQLite column type for NULL.
 *
 * @param[in] stmt Non-null live prepared statement; caller serializes access and ensures required
 *     row/binding state.
 * @param[in] col Zero-based result-column index valid for the current row; no wrapper bounds check.
 * @return True only when sqlite3_column_type() equals SQLITE_NULL.
 */
inline bool column_is_null(sqlite3_stmt* stmt, int col) {
    return sqlite3_column_type(stmt, col) == SQLITE_NULL;
}

/**
 * @brief Copy current-row blob bytes into owned storage.
 *
 * Calls column_bytes then column_blob; conversion/SQLite errors are not separately reported, and
 * vector allocation exceptions propagate.
 *
 * @param[in] stmt Non-null live prepared statement; caller serializes access and ensures required
 *     row/binding state.
 * @param[in] col Zero-based result-column index valid for the current row; no wrapper bounds check.
 * @return Independent byte vector; empty when the SQLite blob pointer is null or byte count
 *     nonpositive.
 */
inline std::vector<uint8_t> column_blob(sqlite3_stmt* stmt, int col) {
    int size = sqlite3_column_bytes(stmt, col);
    const uint8_t* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, col));
    if (!data || size <= 0)
        return {};
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
/**
 * @brief Noncopyable best-effort transaction guard borrowing one SQLite connection.
 *
 * Commits explicitly and otherwise rolls back on destruction. This is not a rollback-success
 * guarantee: rollback() marks completion after a bounded retry loop even if SQLite remains in a
 * transaction.
 */
class Transaction {
public:
    /**
     * @brief Borrow a connection and begin an immediate write transaction.
     *
     * Connection must outlive the guard and have no incompatible active transaction. Constructor
     * invokes exec(BEGIN IMMEDIATE), which can wait under the connection busy policy or throw
     * GpkgError. Do not concurrently use transaction control on this connection.
     *
     * @param[in] db Non-null live SQLite connection borrowed for this call; caller serializes
     *     transaction/statement use.
     */
    explicit Transaction(sqlite3* db) : db_(db) { exec(db_, "BEGIN IMMEDIATE"); }
    /**
     * @brief Commit an unfinished transaction and mark the guard finished.
     *
     * Already-finished guard is a no-op. Failure throws GpkgError and leaves finished_ false so
     * rollback remains possible; the guard does not begin a replacement transaction.
     */
    void commit() {
        if (!finished_) {
            exec(db_, "COMMIT");
            finished_ = true;
        }
    }
    /**
     * @brief Best-effort reset all live statements and roll back an unfinished transaction.
     *
     * Up to three attempts: resets EVERY statement on the borrowed connection, issues ROLLBACK, then
     * checks autocommit. Marks finished even if all attempts fail; no failure status is returned. Can
     * disrupt other statement cursors, so the guard requires exclusive connection control.
     * Already-finished is a no-op.
     */
    void rollback() {
        if (finished_)
            return;
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
            if (sqlite3_get_autocommit(db_) != 0)
                break;
        }
        finished_ = true;
    }
    /**
     * @brief Attempt best-effort rollback unless already committed/rolled back.
     *
     * Uses rollback() and therefore may reset every statement on the borrowed connection. Does not
     * close the connection or report rollback refusal.
     */
    ~Transaction() { rollback(); }
    /**
     * @brief Disallow copying one transaction cleanup responsibility.
     */
    Transaction(const Transaction&) = delete;
    /**
     * @brief Disallow copy replacement of a transaction guard.
     */
    Transaction& operator=(const Transaction&) = delete;

private:
    sqlite3* db_;
    bool finished_ = false;
};

} // namespace openswmm::gpkg

#endif // OPENSWMM_GPKG_UTILS_HPP
