// SPDX-License-Identifier: Apache-2.0

/**
 * @file      EngineSourceResultStream.cpp
 * @brief     Versioned binary companion streams for complete source result histories.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "EngineSourceResultStream.hpp"
#include "EngineSourceResultHistory.hpp"
#include "CommonSourceStage.hpp"
#include "../../core/SWMMEngine.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace openswmm::twoD {
namespace {
using Archive = detail::CompletedSourceArchive;
using Stream = EngineSourceResultStream;
constexpr uint64_t maxFrame = 64 * 1024 * 1024, marker = 0x31454d4152465353ULL,
                   seed = 14695981039346656037ULL;
/**
 * @brief Enforce an internal source-stream invariant.
 * @param[in] ok True returns normally; false throws.
 * @param[in] message Nonnull diagnostic text.
 * @throws std::runtime_error On false; no file/state rollback occurs in this helper.
 */
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
/**
 * @brief Extend a 64-bit FNV-1a rolling fingerprint with binary bytes.
 * @param[in] h Starting fingerprint, normally the preceding committed prefix digest.
 * @param[in] p Borrowed pointer readable for n bytes; may be null only when n is zero.
 * @param[in] n Number of bytes to consume, including embedded NULs.
 * @return Updated modulo-2^64 fingerprint. No allocation/state changes; not a cryptographic
 * hash.
 */
uint64_t hash(uint64_t h, const char* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<unsigned char>(p[i]);
        h *= 1099511628211ULL;
    }
    return h;
}
/**
 * @brief Extend a rolling stream fingerprint by all bytes of a string.
 * @param[in] h Starting 64-bit fingerprint.
 * @param[in] s Stable borrowed binary string.
 * @return FNV-1a continuation, including embedded NULs; not an authentication/security hash.
 */
uint64_t hash(uint64_t h, const std::string& s) {
    return hash(h, s.data(), s.size());
}
struct File {
    FILE* f = nullptr;
    /**
     * @brief Open an owned unbuffered C stream for synchronous companion I/O.
     * @param[in] path Platform-native filesystem path.
     * @param[in] mode Nonnull fopen mode; caller selects read/update or exclusive-create
     * semantics.
     * @throws std::runtime_error When fopen fails. setvbuf return is unchecked; destructor
     * closes the stream, but does not remove its path. This aggregate must not be copied into
     * multiple FILE owners.
     */
    explicit File(const std::string& path, const char* mode) : f(std::fopen(path.c_str(), mode)) {
        require(f, "Cannot open source stream.");
        std::setvbuf(f, nullptr, _IONBF, 0);
    }
    /**
     * @brief Close a still-owned C stream, ignoring fclose errors.
     * @note Publication/append paths explicitly synchronize before this cleanup; destructor
     * alone does not prove successful durable write or remove a staging artifact.
     */
    ~File() {
        if (f)
            std::fclose(f);
    }
};
/**
 * @brief Read exactly a requested binary byte count at the current stream position.
 * @param[in,out] f Borrowed valid readable FILE; advances its cursor by bytes actually read.
 * @param[out] p Writable caller buffer for at least n bytes; partial contents may change on
 * failure.
 * @param[in] n Requested byte count.
 * @throws std::runtime_error On short/truncated/error read. No rollback or stream close occurs.
 */
void read(FILE* f, void* p, std::size_t n) {
    require(std::fread(p, 1, n, f) == n, "Source stream prefix is truncated or unreadable.");
}
/**
 * @brief Write all bytes of a binary string at the current stream position.
 * @param[in,out] f Borrowed valid writable FILE; position/file content can change before
 * failure.
 * @param[in] s Stable borrowed bytes, including embedded NULs.
 * @throws std::runtime_error On incomplete write; caller handles synchronization and rollback.
 */
void write(FILE* f, const std::string& s) {
    require(std::fwrite(s.data(), 1, s.size(), f) == s.size(), "Cannot write source stream frame.");
}
/**
 * @brief Flush the C stream and synchronize its underlying file descriptor.
 * @param[in,out] f Borrowed writable FILE with valid descriptor.
 * @throws std::runtime_error On fflush or platform _commit/fsync failure. Earlier bytes may
 * already be written/synchronized; no parent-directory transaction or rollback is supplied here.
 */
void sync(FILE* f) {
    require(std::fflush(f) == 0, "Cannot flush source stream.");
#ifdef _WIN32
    require(_commit(_fileno(f)) == 0, "Cannot synchronize source stream.");
#else
    require(fsync(fileno(f)) == 0, "Cannot synchronize source stream.");
#endif
}
/**
 * @brief Set a stream's absolute byte position within the signed-64-bit offset range.
 * @param[in,out] f Borrowed valid seekable FILE.
 * @param[in] n Byte offset from start, at most INT64_MAX.
 * @throws std::runtime_error On excessive offset or platform seek failure. No file bytes are
 * changed.
 */
void seek(FILE* f, uint64_t n) {
#ifdef _WIN32
    require(n <= INT64_MAX && _fseeki64(f, static_cast<int64_t>(n), SEEK_SET) == 0,
            "Cannot seek source stream.");
#else
    require(n <= INT64_MAX && fseeko(f, static_cast<off_t>(n), SEEK_SET) == 0,
            "Cannot seek source stream.");
#endif
}
/**
 * @brief Resize a companion file to a prior committed byte boundary.
 * @param[in,out] f Borrowed writable FILE with valid descriptor; not closed or repositioned
 * here.
 * @param[in] n Desired byte length, within the platform supported range (INT64_MAX on POSIX).
 * @throws std::runtime_error On platform resize failure. Destructive tail removal is
 * intentional; caller owns prior cursor verification and any subsequent synchronization.
 */
void truncate(FILE* f, uint64_t n) {
#ifdef _WIN32
    require(_chsize_s(_fileno(f), n) == 0, "Cannot roll back uncommitted source frame.");
#else
    require(n <= INT64_MAX && ftruncate(fileno(f), static_cast<off_t>(n)) == 0,
            "Cannot roll back uncommitted source frame.");
#endif
}
struct Identity {
    uint64_t device, inode, size;
};
/**
 * @brief Inspect a non-symlink regular companion file's device, inode and byte size.
 * @param[in] path Stable existing filesystem path.
 * @param[in] writable True additionally requires at least one owner/group/other write permission
 * bit; this is not a guarantee against ACL, sharing or filesystem write refusal.
 * @return Copied native device/inode/size identity.
 * @throws std::runtime_error On nonregular/symlink path, permission-bit refusal or stat failure;
 * filesystem exceptions can propagate. No file is opened or mutated; caller excludes path races.
 */
Identity identity(const std::string& path, bool writable = false) {
    auto status = std::filesystem::symlink_status(path);
    require(std::filesystem::is_regular_file(status),
            "Source stream must be a regular file, not a symlink.");
    if (writable)
        require((status.permissions() &
                 (std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                  std::filesystem::perms::others_write)) != std::filesystem::perms::none,
                "Source stream is not writable.");
#ifdef _WIN32
    struct _stat64 s{};
    require(_wstat64(std::filesystem::path(path).c_str(), &s) == 0,
            "Cannot inspect source stream.");
#else
    struct stat s{};
    require(stat(path.c_str(), &s) == 0, "Cannot inspect source stream.");
#endif
    return {uint64_t(s.st_dev), uint64_t(s.st_ino), uint64_t(s.st_size)};
}
/**
 * @brief Archive at most 64 named diagnostic tables with their explicit field/unit/column
 * vectors.
 * @param[in,out] a Sequential archive; errors/allocations propagate after partial decoding.
 * @param[in,out] ts Owned table map to encode or clear/rebuild on read. Duplicate decoded names
 * are refused. Value shape/finite/profile checks are performed separately by shape()/Progress.
 * @throws std::runtime_error On excessive table count or duplicate decoded table key.
 * @note No complete rollback; decode into a private frame. Input file I/O is outside this
 * helper.
 */
void tables(Archive& a, std::map<std::string, SourceResultTable>& ts) {
    uint64_t size = ts.size();
    a(size);
    require(size <= 64, "Too many source stream tables.");
    if (a.reading())
        ts.clear();
    if (a.reading())
        for (uint64_t i = 0; i < size; ++i) {
            std::string name;
            SourceResultTable t;
            a(name, t.fields, t.units, t.columns);
            require(ts.emplace(std::move(name), std::move(t)).second,
                    "Duplicate source stream table.");
        }
    else
        for (auto& [name, t] : ts) {
            auto key = name;
            a(key, t.fields, t.units, t.columns);
        }
}
/**
 * @brief Validate generic frame clocks, table schemas and bounded finite numeric columns.
 * @param[in] f Stable decoded/projected frame.
 * @throws std::runtime_error On unsupported Kind, empty table set, nonfinite/negative/reversed
 * clocks, empty/oversized names/schema mismatch, unequal row lengths or more than 8*1024*1024
 * numeric values.
 * @note Does not validate inter-frame progression or all physical receipt semantics;
 * Progress::accept() and the model projection handle those. No input mutation, file access or
 * allocation is required.
 */
void shape(const Stream::Frame& f) {
    require(uint32_t(f.kind) <= uint32_t(Stream::Kind::Checkpoint) && !f.tables.empty(),
            "Invalid source stream capture kind or tables.");
    require(std::isfinite(f.engine_start) && std::isfinite(f.engine_end) &&
                std::isfinite(f.source_start) && std::isfinite(f.source_end) &&
                f.engine_start >= 0 && f.engine_end >= f.engine_start && f.source_start >= 0 &&
                f.source_end >= f.source_start,
            "Invalid source stream clocks.");
    std::size_t values = 0;
    for (const auto& [name, t] : f.tables) {
        require(!name.empty() && name.size() < 128 && t.fields.size() == t.units.size() &&
                    t.fields.size() == t.columns.size() && !t.columns.empty(),
                "Invalid source stream table schema.");
        for (std::size_t c = 0; c < t.columns.size(); ++c) {
            require(!t.fields[c].empty() && t.fields[c].size() < 128 && t.units[c].size() < 128 &&
                        t.columns[c].size() == t.rows() && t.rows() <= 8 * 1024 * 1024 - values,
                    "Invalid or oversized source stream column.");
            values += t.rows();
            for (double v : t.columns[c])
                require(std::isfinite(v), "Nonfinite source stream value.");
        }
    }
}
/**
 * @brief Validate and encode one generic source-stream frame payload.
 * @param[in,out] f Stable frame visited by the archive writer; no intended field mutation.
 * @return Owned binary payload, at most 64 MiB, excluding record length/hash/commit-marker
 * framing.
 * @throws std::runtime_error On invalid shape or excessive encoded payload; allocation errors
 * propagate. Inter-frame sequence/profile checks belong to Progress::accept().
 */
std::string encode(Stream::Frame& f) {
    shape(f);
    Archive a;
    a(f.kind, f.sequence, f.source_intervals, f.boundary_fingerprint, f.engine_start, f.engine_end,
      f.source_start, f.source_end);
    tables(a, f.tables);
    require(a.bytes().size() <= maxFrame, "Source stream frame exceeds its size limit.");
    return a.bytes();
}
/**
 * @brief Decode a complete frame payload and validate its generic bounded shape.
 * @param[in] bytes Owned payload; archive framing/trailing bytes must exactly match one frame.
 * @return Owned Frame with finite clocks and consistent bounded tables.
 * @throws std::runtime_error On archive/trailing bytes/shape failure. Allocation errors
 * propagate; no stream state or caller frame is changed. Sequence/rolling-hash checks belong to
 * the scanner.
 */
Stream::Frame decode(std::string bytes) {
    Archive a(std::move(bytes));
    Stream::Frame f;
    a(f.kind, f.sequence, f.source_intervals, f.boundary_fingerprint, f.engine_start, f.engine_end,
      f.source_start, f.source_end);
    tables(a, f.tables);
    a.finish();
    shape(f);
    return f;
}
/**
 * @brief Encode a versioned native-platform stream header with a byte-count prefix.
 * @param[in] id Stream identifier string, normally 32 hexadecimal characters from
 * initialization.
 * @param[in] metadata Exact binary model/source/node/profile identity payload; borrowed while
 * copied.
 * @return Owned uint64-size-prefixed header bytes; encoded body is at most 64 MiB.
 * @throws std::runtime_error On excessive metadata/header size. This writer does not
 * independently check id length or metadata emptiness; readHeader() enforces those. No file is
 * published.
 */
std::string header(const std::string& id, const std::string& metadata) {
    Archive a;
    std::string magic = "OpenSWMM source stream", identity = id, model = metadata;
    uint32_t version = 1, endian = 0x01020304;
    a(magic, version, endian, identity, model);
    uint64_t size = a.bytes().size();
    require(size <= maxFrame, "Source stream metadata exceeds its size limit.");
    Archive out;
    out(size);
    return out.bytes() + a.bytes();
}
struct Header {
    std::string identity, metadata;
    uint64_t bytes = 0, fingerprint = 0;
};
/**
 * @brief Read and validate a source-stream header at the current input position.
 * @param[in,out] f Borrowed readable FILE positioned at the header start; advances on partial
 * reads.
 * @return Owned identity/metadata, total header byte count and rolling fingerprint including its
 * native uint64 length prefix.
 * @throws std::runtime_error On truncated/oversized body, wrong magic/version/endian, identity
 * length other than 32 or empty metadata. Does not prove hexadecimal randomness or model
 * compatibility. No file content changes; allocation failures propagate.
 */
Header readHeader(FILE* f) {
    uint64_t size;
    read(f, &size, sizeof(size));
    require(size > 0 && size <= maxFrame, "Invalid source stream header size.");
    std::string bytes(size, '\0');
    read(f, bytes.data(), bytes.size());
    Archive a(bytes);
    Header h;
    std::string magic;
    uint32_t version, endian;
    a(magic, version, endian, h.identity, h.metadata);
    a.finish();
    require(magic == "OpenSWMM source stream" && version == 1 && endian == 0x01020304 &&
                h.identity.size() == 32 && !h.metadata.empty(),
            "Source stream format or identity differs.");
    h.bytes = sizeof(size) + size;
    h.fingerprint = hash(hash(seed, reinterpret_cast<const char*>(&size), sizeof(size)), bytes);
    return h;
}
struct Progress {
    uint64_t frames = 0, intervals = 0;
    double engine_end = 0, source_end = 0;
    bool detailed = false;
    std::map<std::string, SourceResultTable> schemas;
    /**
     * @brief Validate and advance a private inter-frame source/engine progression cursor.
     * @param[in] f Stable shape-validated frame with exact table/clock/provenance fields.
     * @throws std::runtime_error On noncontiguous sequence/counts beyond 2^53-1, wrong
     * initial/engine/ barrier/source progression, schema additions/omissions/units, missing
     * boundary row or replayed barrier routing receipts.
     * @note Mutates this Progress's schemas/clocks/counts during checking and may leave partial
     * state on failure; use a private validator. Detailed source frames advance source clocks
     * while holding engine time, and later engine frames must match that horizon. No
     * file/physical state is changed.
     */
    void accept(const Stream::Frame& f) {
        require(f.sequence == frames && frames < 9007199254740991ULL &&
                    f.source_intervals <= 9007199254740991ULL,
                "Source stream sequence is not contiguous or exceeds exact table precision.");
        if (!frames)
            for (const auto& [name, t] : f.tables) {
                SourceResultTable schema;
                schema.fields = t.fields;
                schema.units = t.units;
                schema.columns.resize(t.columns.size());
                schemas.emplace(name, std::move(schema));
            }
        else if (!schemas.empty()) {
            std::size_t expected = 0;
            const bool source = f.kind == Stream::Kind::SourceInterval;
            for (const auto& [name, t] : schemas) {
                if (source ? (!name.starts_with("source/") || name == "source/footprints" ||
                              name == "source/lid_usages")
                           : !name.starts_with("engine/"))
                    continue;
                ++expected;
                const auto found = f.tables.find(name);
                require(found != f.tables.end() && found->second.fields == t.fields &&
                            found->second.units == t.units,
                        "Source stream table fields or units changed.");
            }
            require(f.tables.size() == expected, "Source stream frame omits or adds tables.");
        }
        if (!frames) {
            require(f.kind == Stream::Kind::Initial && f.engine_start == 0 && f.engine_end == 0 &&
                        f.source_start == 0 && f.source_end == 0 && f.source_intervals == 0 &&
                        f.boundary_fingerprint != 0,
                    "Invalid initial source stream frame.");
        } else if (f.kind == Stream::Kind::SourceInterval) {
            require(detailed && f.engine_start == engine_end && f.engine_end == engine_end &&
                        f.source_start == source_end && f.source_end > source_end &&
                        f.source_intervals == intervals + 1 && f.boundary_fingerprint == 0,
                    "Invalid streamed source interval progression.");
            intervals = f.source_intervals;
            source_end = f.source_end;
        } else {
            require(f.kind != Stream::Kind::Initial && f.engine_start == engine_end &&
                        f.boundary_fingerprint != 0 &&
                        (f.kind == Stream::Kind::EngineStep ? f.engine_end > engine_end
                                                            : f.engine_end == engine_end) &&
                        f.source_end <= f.engine_end + 1e-12 * std::max(1., f.engine_end),
                    "Invalid streamed engine step or barrier.");
            require(detailed
                        ? f.source_intervals == intervals && f.source_end == source_end
                        : f.source_intervals >= intervals && f.source_end >= source_end &&
                              ((f.source_intervals == intervals) == (f.source_end == source_end)),
                    "Streamed source/engine coverage differs.");
            intervals = f.source_intervals;
            source_end = f.source_end;
            engine_end = f.engine_end;
        }
        const bool source = f.kind == Stream::Kind::SourceInterval;
        const auto found = f.tables.find(source ? "source/boundaries" : "engine/boundaries");
        require(found != f.tables.end() && found->second.rows() == 1,
                "Source stream boundary table is missing.");
        const auto& b = found->second.columns;
        require(b.size() == (source ? 7 : 10), "Source stream boundary schema differs.");
        require(b[0][0] == double(source ? f.source_intervals : f.sequence),
                "Source stream table sequence differs.");
        if (source)
            require(b.size() == 7 && b[1][0] == double(f.source_intervals) &&
                        b[2][0] == f.source_start && b[3][0] == f.source_end,
                    "Source stream interval table clocks differ.");
        else
            require(b.size() == 10 && b[1][0] == f.engine_start && b[2][0] == f.engine_end &&
                        b[3][0] == double(f.source_intervals) && b[4][0] == f.source_end,
                    "Source stream engine table clocks differ.");
        if (!source && f.kind != Stream::Kind::EngineStep) {
            const auto& n = f.tables.at("engine/node_delivery").columns;
            require(n.size() == 11, "Source stream node delivery schema differs.");
            for (std::size_t r = 0; r < n[0].size(); ++r)
                require(n[6][r] == 0 && n[7][r] == 0 && n[8][r] == f.engine_end &&
                            n[9][r] == f.engine_end && n[10][r] == 0,
                        "Barrier replays an earlier routing receipt.");
        }
        ++frames;
    }
};
struct Scan {
    Header header;
    Stream::Frame latest;
    std::size_t largest = 0;
    Progress progress;
};
/**
 * @brief Stream-verify an exact saved prefix while retaining only its latest bounded frame.
 * @param[in] path Stable non-symlink regular companion file; size must be at least cursor.bytes.
 * @param[in] cursor Nonempty expected identity with positive frame count, exact committed byte
 * boundary and rolling fingerprint. Bytes after that boundary are deliberately ignored.
 * @param[in] visit Optional synchronous callback for each decoded/progression-validated frame.
 * References expire at callback return; effects survive any later frame/final-cursor failure.
 * @return Owned header/latest frame, largest payload byte count and final progression state.
 * @throws std::runtime_error On file/header/model-metadata framing, truncated/bad record length,
 * rolling chain/commit-marker, frame/schema/clock or final cursor mismatch. Callback/allocation
 * exceptions propagate; no file or engine state changes.
 * @note Reads one <=64 MiB payload at a time; not a cryptographic integrity/authenticity check.
 * No lock is acquired, so caller excludes concurrent modification/path replacement.
 */
Scan scan(const std::string& path, const Stream::Cursor& cursor,
          const std::function<void(const Stream::Frame&)>& visit = {}) {
    require(!cursor.identity.empty() && cursor.frames > 0 && identity(path).size >= cursor.bytes,
            "Source stream saved prefix is missing or truncated.");
    File file(path, "rb");
    Scan out;
    out.header = readHeader(file.f);
    require(out.header.identity == cursor.identity && out.header.bytes < cursor.bytes,
            "Source stream identity differs.");
    {
        Archive a(out.header.metadata);
        std::string model;
        std::vector<std::string> sources, nodes;
        a(model, sources, nodes, out.progress.detailed);
        a.finish();
    }
    uint64_t bytes = out.header.bytes, digest = out.header.fingerprint;
    while (bytes < cursor.bytes) {
        require(cursor.bytes - bytes >= 32, "Uncommitted source stream frame.");
        uint64_t size, previous, stored, end;
        read(file.f, &size, 8);
        read(file.f, &previous, 8);
        require(size > 0 && size <= maxFrame && size <= cursor.bytes - bytes - 32 &&
                    previous == digest,
                "Invalid source stream frame length or prefix fingerprint.");
        std::string payload(size, '\0');
        read(file.f, payload.data(), payload.size());
        read(file.f, &stored, 8);
        read(file.f, &end, 8);
        digest = hash(hash(digest, reinterpret_cast<const char*>(&size), 8), payload);
        require(end == marker && stored == digest,
                "Source stream frame checksum or commit marker differs.");
        auto frame = decode(std::move(payload));
        out.progress.accept(frame);
        if (visit)
            visit(frame);
        out.latest = std::move(frame);
        out.largest = std::max(out.largest, std::size_t(size));
        bytes += size + 32;
    }
    require(bytes == cursor.bytes && out.progress.frames == cursor.frames &&
                digest == cursor.fingerprint,
            "Source stream committed cursor differs.");
    return out;
}
/**
 * @brief Move projected tables into a namespaced frame map.
 * @param[in,out] dest Destination; existing same-name keys win and are not overwritten.
 * @param[in] source Owned table map moved from one projection.
 * @param[in] prefix Nonnull namespace prefix, normally engine/ or source/.
 * @note Allocation errors can leave an inserted prefix; no duplicate-key diagnostic is produced.
 * Callers construct a private frame before append/publication.
 */
void merge(std::map<std::string, SourceResultTable>& dest,
           std::map<std::string, SourceResultTable> source, const char* prefix) {
    for (auto& [name, t] : source)
        dest.emplace(std::string(prefix) + name, std::move(t));
}
}
/**
 * @brief Compute the stream's native binary FNV-1a identity fingerprint.
 * @param[in] bytes Stable binary string, including embedded NULs.
 * @return Seeded 64-bit fingerprint, deterministic for these bytes; not a cryptographic hash. No
 * allocation or state mutation occurs.
 */
uint64_t EngineSourceResultStream::fingerprint(const std::string& bytes) {
    return hash(seed, bytes);
}
/**
 * @brief Encode all four saved-prefix cursor fields in native internal archive order.
 * @param[in] c Stable cursor value.
 * @return Owned binary string of identity, bytes, frames and rolling fingerprint. No
 * semantic/file verification occurs; allocation/archive errors propagate and c remains
 * unchanged.
 */
std::string EngineSourceResultStream::encodeCursor(const Cursor& c) {
    auto copy = c;
    Archive a;
    a(copy.identity, copy.bytes, copy.frames, copy.fingerprint);
    return a.bytes();
}
/**
 * @brief Decode exactly one saved-prefix cursor envelope.
 * @param[in] bytes Owned internal cursor bytes; trailing bytes are rejected.
 * @return Owned Cursor; semantic identity/file/count/fingerprint checks belong to scan().
 * @throws std::runtime_error On malformed/trailing archive bytes; allocation errors propagate.
 */
EngineSourceResultStream::Cursor EngineSourceResultStream::decodeCursor(std::string bytes) {
    Archive a(std::move(bytes));
    Cursor c;
    a(c.identity, c.bytes, c.frames, c.fingerprint);
    a.finish();
    return c;
}
/**
 * @brief Visit and verify a committed saved prefix without retaining complete history.
 * @param[in] path Stable non-symlink regular source companion file, at least cursor.bytes long.
 * @param[in] cursor Exact saved identity, byte/frame count and rolling fingerprint.
 * @param[in] visitor Optional synchronous callback for each verified decoded frame; references
 * expire at callback return. Callback effects survive later verification failure and must not
 * mutate the file.
 * @return Empty after complete prefix verification, or file/schema/clock/cursor/callback
 * standard exception text. Empty callback still scans; extra bytes beyond the cursor are
 * ignored.
 * @note No file mutation, hydraulic restoration, synchronization or cryptographic authenticity
 * check. Caller serializes source access; nonstandard callback exceptions can propagate.
 */
std::string EngineSourceResultStream::visit(const std::string& path, const Cursor& cursor,
                                            const std::function<void(const Frame&)>& visitor) {
    try {
        scan(path, cursor, visitor);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
/**
 * @brief Verify the live append destination still matches its committed identity and tail
 * marker.
 * @throws std::runtime_error On path/type/write-bit, device/inode, exact size or final
 * digest/marker mismatch. Extra uncommitted tail bytes are refused, not silently removed.
 * @note Does not recompute earlier prefix hashes or model metadata; checkpointContract()/scan()
 * perform full verification. Requires attached path/cursor and caller-serialized file/recorder
 * access. Read-only filesystem inspection may allocate or throw.
 */
void EngineSourceResultStream::preflight() const {
    const auto now = identity(path_, true);
    require(now.device == device_ && now.inode == inode_ && now.size == cursor_.bytes,
            "Source stream destination changed, was truncated, or has an uncommitted tail.");
    if (cursor_.frames) {
        File f(path_, "rb");
        seek(f.f, cursor_.bytes - 16);
        uint64_t digest, end;
        read(f.f, &digest, 8);
        read(f.f, &end, 8);
        require(digest == cursor_.fingerprint && end == marker,
                "Source stream final commit marker changed.");
    }
}
/**
 * @brief Encode, append/synchronize and then publish one committed frame/cursor.
 * @param[in] frame Owned next frame with matching sequence/progression and valid projection
 * schema.
 * @throws std::runtime_error On preflight/progression/encoding/write/sync refusal; allocation
 * errors may propagate before writing. Write/sync failure attempts truncation to the old cursor
 * and resync, but rollback failures are swallowed and can leave a tail requiring inspection.
 * @note In-memory cursor/latest/clocks/largest payload update only after synchronized record
 * write. Framing chains length+payload fingerprints and a commit marker. This local append
 * preflight does not rescan all previous tables for schema identity; full-prefix scan does. No
 * physics rollback, interprocess lock or whole-output transaction is supplied; caller serializes
 * recorder/file access.
 */
void EngineSourceResultStream::append(Frame frame) {
    preflight();
    Progress progress{cursor_.frames, source_intervals_, engine_end_, source_end_, detailed_};
    progress.accept(frame);
    const auto payload = encode(frame);
    uint64_t size = payload.size(), previous = cursor_.fingerprint;
    const auto digest = hash(hash(previous, reinterpret_cast<const char*>(&size), 8), payload);
    uint64_t end = marker;
    Archive a;
    a(size, previous);
    std::string record = a.bytes() + payload;
    Archive footer;
    auto saved = digest;
    footer(saved, end);
    record += footer.bytes();
    require(record.size() <= UINT64_MAX - cursor_.bytes, "Source stream byte cursor overflow.");
    Cursor next = cursor_;
    next.bytes += record.size();
    ++next.frames;
    next.fingerprint = digest; // allocate before writing
    File f(path_, "rb+");
    seek(f.f, cursor_.bytes);
    try {
        write(f.f, record);
        sync(f.f);
    } catch (...) {
        std::clearerr(f.f);
        try {
            std::fflush(f.f);
            truncate(f.f, cursor_.bytes);
            sync(f.f);
        } catch (...) {
        }
        throw;
    }
    cursor_ = std::move(next);
    latest_ = std::move(frame);
    engine_end_ = progress.engine_end;
    source_end_ = progress.source_end;
    source_intervals_ = progress.intervals;
    largest_frame_ = std::max(largest_frame_, payload.size());
}
/**
 * @brief Exclusively create and attach a fresh companion stream at engine time zero.
 * - e (in): Stable prepared engine with common source stage and managed bridge or LID receipts;
 * its address/context/model remain borrowed through stream lifetime, which the caller retains to
 * close.
 * - path (in): New destination; existing files are not truncated. Parent directory must exist.
 * - detailed (in): True retains fine source-interval frames in addition to engine/barrier
 * frames.
 * @return Empty after synchronized model header, initial engine/source frame and fresh
 * publication guard capture; otherwise caught standard-exception text. A file created by this
 * call is removed best-effort on reported failure and attachment/path/cursor/latest are cleared.
 * @note Refuses an existing stream attachment or nonzero engine time. Uses random 32-character
 * identity, native-platform versioned archives and copied names/model metadata. Failure does not
 * promise every auxiliary status/statistic field is reset; no hydraulic state is advanced.
 */
std::string EngineSourceResultStream::initialize(const SWMMEngine& e, const std::string& path,
                                                 bool detailed) {
    bool created = false;
    try {
        require(!engine_ && path_.empty() && e.ctx_.current_time == 0 &&
                    e.surface_router_.commonSourceStage() &&
                    (e.source_delivery_ || e.surface_router_.subsurface().lidReceiptsEnabled()),
                "Attach an empty stream at the initial spatial-source checkpoint.");
        std::vector<std::string> sources, nodes;
        for (int sc = 0; sc < e.ctx_.n_subcatches(); ++sc)
            sources.push_back(e.ctx_.subcatch_names.name_of(sc));
        for (int n = 0; n < e.ctx_.n_nodes(); ++n)
            nodes.push_back(e.ctx_.node_names.name_of(n));
        Archive meta;
        auto model = e.checkpoint_model_;
        meta(model, sources, nodes, detailed);
        metadata_ = meta.bytes();
        std::random_device random;
        const char* hex = "0123456789abcdef";
        std::string id;
        id.reserve(32);
        for (int i = 0; i < 32; ++i)
            id += hex[random() & 15];
        auto bytes = header(id, metadata_);
        path_ = path;
        {
            File f(path, "wbx");
            created = true;
            write(f.f, bytes);
            sync(f.f);
        }
        auto named = identity(path);
        device_ = named.device;
        inode_ = named.inode;
        cursor_ = {id, uint64_t(bytes.size()), 0, fingerprint(bytes)};
        engine_ = &e;
        detailed_ = detailed;
        const auto error = capture(e, Kind::Initial, 0);
        require(error.empty(), error.c_str());
        // The fresh destination contains one complete initial frame, bounded by model size.
        File f(path, "rb");
        fresh_header_.resize(cursor_.bytes);
        read(f.f, fresh_header_.data(), fresh_header_.size());
        return {};
    } catch (const std::exception& e) {
        if (created) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        path_.clear();
        metadata_.clear();
        fresh_header_.clear();
        cursor_ = {};
        latest_ = {};
        engine_ = nullptr;
        return e.what();
    }
}
/**
 * @brief Record an accepted engine step or zero-time barrier with its physical checkpoint
 * anchor.
 * - e (in): Exact attached live engine with common source stage and managed bridge/LID receipts.
 * - kind (in): Initial, EngineStep, CouplingBarrier, PrescriptionUpdate or Checkpoint as
 * appropriate; SourceInterval belongs to captureSourceInterval(). Progression validation refuses
 * invalid use.
 * - start (in): Accepted elapsed engine start s; barriers hold start=end and EngineStep advances
 * it.
 * - anchor (in): Optional nonzero supplied physical-checkpoint fingerprint. Zero computes it
 * from engine checkpointBytes(...,false); canonical declaration names this parameter
 * fingerprint.
 * @return Empty after synchronized append, clearing pending flag/error; caught
 * standard-exception message otherwise, stored in error_. Failure after obtaining an anchor
 * leaves capture pending.
 * @note Builds immutable diagnostic tables, suppresses replay of prior routing receipts at
 * barriers, and includes initial source ownership tables only at Initial. Physics/prescriptions
 * may already have been accepted; capture failure does not roll them back. Caller must settle
 * recording before advancing and serialize all stream/engine access. Early failures may precede
 * the pending flag.
 */
std::string EngineSourceResultStream::capture(const SWMMEngine& e, Kind kind, double start,
                                              uint64_t anchor) {
    try {
        require(engine_ == &e && e.surface_router_.commonSourceStage() &&
                    (e.source_delivery_ || e.surface_router_.subsurface().lidReceiptsEnabled()),
                "Source stream belongs to a different engine.");
        if (!anchor) {
            std::string bytes;
            const auto error = e.checkpointBytes(bytes, false);
            require(error.empty(), error.c_str());
            anchor = fingerprint(bytes);
        }
        pending_capture_ = true;
        pending_kind_ = kind;
        pending_start_ = start;
        Frame frame;
        frame.kind = kind;
        frame.sequence = cursor_.frames;
        frame.engine_start = start;
        frame.engine_end = e.ctx_.current_time;
        frame.source_start = source_end_;
        frame.source_end = e.surface_router_.commonSourceStage()->completedEnd();
        frame.source_intervals = e.surface_router_.commonSourceStage()->intervals();
        frame.boundary_fingerprint = anchor;
        auto projected = EngineSourceResultHistory::projection(e, frame.sequence, start);
        if (kind != Kind::EngineStep) {
            auto& n = projected.tables_.at("node_delivery").columns;
            for (std::size_t r = 0; r < n[0].size(); ++r) {
                n[6][r] = n[7][r] = n[10][r] = 0;
                n[8][r] = n[9][r] = frame.engine_end;
            }
        }
        merge(frame.tables, std::move(projected.tables_), "engine/");
        if (kind == Kind::Initial) {
            auto source =
                SourceResultHistory::projection(*e.surface_router_.commonSourceStage(), 0, 0);
            merge(frame.tables, std::move(source.tables_), "source/");
        }
        append(std::move(frame));
        pending_capture_ = false;
        error_.clear();
        return {};
    } catch (const std::exception& e) {
        error_ = e.what();
        return error_;
    }
}
/**
 * @brief Append a fine accepted source interval while holding the last committed engine clock.
 * @param[in] stage Exact common source stage of this attached detailed-profile engine, stable
 * through synchronous projection. New source interval must follow the saved source
 * horizon/count.
 * @return Empty after synchronized append; caught standard-exception diagnostic stored in error_
 * otherwise. A successful call does not clear an earlier error_ or alter pending_capture_.
 * @note Omits repeated static source footprints/usages; uses native source receipts on their own
 * elapsed-second axis. No hydraulic/driver state is changed or rolled back; caller orders this
 * within accepted-step work and excludes other file/recorder operations.
 */
std::string EngineSourceResultStream::captureSourceInterval(const CommonSourceStage& stage) {
    try {
        require(detailed_ && engine_ && engine_->surface_router_.commonSourceStage() == &stage,
                "Source interval requires its attached detailed stream.");
        Frame frame;
        frame.kind = Kind::SourceInterval;
        frame.sequence = cursor_.frames;
        frame.engine_start = frame.engine_end = engine_end_;
        frame.source_start = source_end_;
        frame.source_end = stage.completedEnd();
        frame.source_intervals = stage.intervals();
        auto source =
            SourceResultHistory::projection(stage, frame.source_intervals, frame.source_start);
        source.tables_.erase("footprints");
        source.tables_.erase("lid_usages");
        merge(frame.tables, std::move(source.tables_), "source/");
        append(std::move(frame));
        return {};
    } catch (const std::exception& e) {
        error_ = e.what();
        return error_;
    }
}
/**
 * @brief Verify and bind the committed stream prefix to physical and containing checkpoint
 * fingerprints.
 * @param[in] physical Required exact nonzero boundary fingerprint of latest non-source-interval
 * frame.
 * @param[in] whole Fingerprint of the whole-engine/output checkpoint owning this stream
 * contract.
 * @return Owned binary archive of saved path, encoded cursor and both fingerprints.
 * @throws std::runtime_error On unsettled/mismatching latest boundary, destination preflight or
 * full prefix scan failure. Allocation errors propagate. No file write or physical state change
 * occurs.
 * @note Fingerprints bind exact bytes for same-platform continuation; they are not security
 * hashes.
 */
std::string EngineSourceResultStream::checkpointContract(uint64_t physical, uint64_t whole) const {
    require(latest_.kind != Kind::SourceInterval && latest_.boundary_fingerprint == physical,
            "Stream does not match the accepted physical checkpoint; settle recording first.");
    preflight();
    scan(path_, cursor_);
    Archive a;
    auto path = path_, cursor = encodeCursor(cursor_);
    a(path, cursor, physical, whole);
    return a.bytes();
}
/**
 * @brief Best-effort remove this plan's exclusively owned unadopted staging file.
 * @note Does nothing after adopt() disarms owns_staging. Does not publish/restore engine or live
 * stream state and ignores filesystem removal errors.
 */
EngineSourceResultStream::PreparedRestore::~PreparedRestore() {
    if (owns_staging) {
        std::error_code ec;
        std::filesystem::remove(publication.staging, ec);
    }
}
/**
 * @brief Verify/copy a saved committed prefix into an owned publication plan for a fresh
 * destination.
 * @param[in] contract Exact checkpointContract() archive, including source path/cursor/physical
 * and whole-engine binding. It must decode without trailing bytes.
 * @param[in] whole_checkpoint Required containing whole-engine/output fingerprint matching
 * contract.
 * @return Owned plan with staged prefix, verified cursor/latest frame, file identity and largest
 * payload. Abandonment removes staging; actual publication and adopt() occur later.
 * @throws std::runtime_error On nonfresh destination, binding/model/profile mismatch,
 * source/destination path alias, source prefix corruption or copy/sync/verification failure.
 * Allocation/I/O errors propagate.
 * @note Requires attached engine at end zero with exactly one initial frame. Rechecks
 * destination preflight, scans source, copies only cursor.bytes in 64 KiB blocks to an exclusive
 * sibling, then rescans the copy to detect source changes. Increments a mutable preparation name
 * counter, but leaves live stream clocks/cursor/physics untouched. Caller excludes concurrent
 * path/file changes.
 */
std::unique_ptr<EngineSourceResultStream::PreparedRestore>
EngineSourceResultStream::prepareRestore(const std::string& contract,
                                         uint64_t whole_checkpoint) const {
    require(engine_ && engine_end_ == 0 && cursor_.frames == 1,
            "Stream restore requires its fresh initial destination.");
    preflight();
    Archive a(contract);
    std::string source, saved;
    uint64_t anchor, whole;
    a(source, saved, anchor, whole);
    a.finish();
    require(whole == whole_checkpoint,
            "Stream saved prefix is bound to a different whole-engine/output checkpoint.");
    const auto cursor = decodeCursor(std::move(saved));
    auto verified = scan(source, cursor);
    require(verified.header.metadata == metadata_ && verified.latest.kind != Kind::SourceInterval &&
                verified.latest.boundary_fingerprint == anchor,
            "Stream identity/model/profile or physical checkpoint binding differs.");
    require(std::filesystem::weakly_canonical(source) != std::filesystem::weakly_canonical(path_) &&
                !std::filesystem::equivalent(source, path_),
            "Stream restore source aliases its destination.");
    auto prepared = std::make_unique<PreparedRestore>();
    prepared->publication = {path_ + ".stream-part-" + cursor_.identity + "-" +
                                 std::to_string(++preparation_sequence_),
                             path_, fresh_header_, -1};
    {
        File in(source, "rb"), out(prepared->publication.staging.string(), "wbx");
        prepared->owns_staging = true;
        std::string buffer(64 * 1024, '\0');
        uint64_t left = cursor.bytes;
        while (left) {
            auto size = std::min(left, uint64_t(buffer.size()));
            read(in.f, buffer.data(), size);
            require(std::fwrite(buffer.data(), 1, size, out.f) == size,
                    "Cannot stage source stream prefix.");
            left -= size;
        }
        sync(out.f);
    }
    // Validate the copied bytes as well, detecting changes during preparation.
    auto copied = scan(prepared->publication.staging.string(), cursor);
    auto named = identity(prepared->publication.staging.string());
    prepared->cursor = cursor;
    prepared->latest = std::move(copied.latest);
    prepared->device = named.device;
    prepared->inode = named.inode;
    prepared->largest_frame = copied.largest;
    return prepared;
}
/**
 * @brief Adopt a successfully published prepared prefix through nonthrowing in-memory moves.
 * @param[in,out] p Valid plan for this stream; its staging cleanup is disarmed, cursor/latest
 * are moved, file identity/largest payload/clocks are copied, pending capture is cleared and
 * error reset.
 * @note Publication must already have succeeded under the containing engine/output transaction.
 * Performs no I/O, schema validation or physics adoption. Caller serializes access and preserves
 * the existing engine/path/model metadata attachment; this method does not take ownership of the
 * engine.
 */
void EngineSourceResultStream::adopt(PreparedRestore& p) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<Cursor> &&
                  std::is_nothrow_move_assignable_v<Frame>);
    p.owns_staging = false;
    cursor_ = std::move(p.cursor);
    latest_ = std::move(p.latest);
    device_ = p.device;
    inode_ = p.inode;
    largest_frame_ = p.largest_frame;
    source_intervals_ = latest_.source_intervals;
    engine_end_ = latest_.engine_end;
    source_end_ = latest_.source_end;
    pending_capture_ = false;
    error_.clear();
}
}
