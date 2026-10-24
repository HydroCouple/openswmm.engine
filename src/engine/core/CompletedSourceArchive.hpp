// SPDX-License-Identifier: Apache-2.0

/**
 * @file      CompletedSourceArchive.hpp
 * @brief     Versioned archives of completed source-boundary state.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace openswmm::detail {
// Internal, same-platform, versioned source-boundary archive. Explicit fields
// only: no object padding, pointers, streams or raw solver objects on disk.
class CompletedSourceArchive {
public:
    /**
     * @brief Create an empty write-mode same-platform byte archive.
     */
    CompletedSourceArchive() = default;
    /**
     * @brief Own a byte string and enter read mode at offset zero.
     *
     * The caller supplies/version-checks the surrounding checkpoint schema. Encoding uses host
     * arithmetic representation/endianness, with no padding or raw pointers.
     *
     * @param[in] bytes Archive bytes passed by value and moved into owned storage; no validation occurs
     *     until fields are consumed.
     */
    explicit CompletedSourceArchive(std::string bytes) : bytes_(std::move(bytes)), reading_(true) {}
    /**
     * @brief Test archive direction.
     * @return True for construction from bytes; false for default write construction.
     */
    bool reading() const { return reading_; }
    /**
     * @brief Borrow the complete owned byte buffer.
     * @return Owned string reference; character views may be invalidated by subsequent writes or
     *     archive destruction.
     */
    const std::string& bytes() const { return bytes_; }
    /**
     * @brief Reject trailing unconsumed bytes after read-mode field visitation.
     *
     * Write mode is a no-op. Does not undo previously decoded fields.
     * @throws std::runtime_error
     * When read offset differs from owned byte length.
     */
    void finish() const {
        if (reading_ && offset_ != bytes_.size())
            fail();
    }
    /**
     * @brief Visit fields in argument order using their supported archive encodings.
     *
     * Decoding mutates destinations incrementally; exceptions leave prior fields consumed/modified.
     * Unsupported types fail at compile time.
     *
     * @param[in,out] values Lvalue fields: arithmetic/enums, strings, vectors, arrays or pairs. Units
     *     and semantic validation belong to the caller.
     */
    template <class... T> void operator()(T&... values) { (field(values), ...); }
    /**
     * @brief Visit vectors while requiring saved lengths to match current destination shapes.
     *
     * Shape mismatches throw rather than resize. Successfully consumed earlier vectors are not rolled
     * back.
     *
     * @param[in,out] values Caller-owned vectors whose read-mode sizes must already equal the saved
     *     lengths.
     */
    template <class... T> void fixed(T&... values) { (fixedOne(values), ...); }
    template <class T, class F>
    /**
     * @brief Visit a vector count and then each element through a supplied visitor.
     *
     * Charges count*sizeof(T) to a cumulative 64 MiB logical-allocation budget in both directions; on
     * read also requires count<=remaining bytes. Visitor/resize exceptions propagate with partial
     * state.
     * @throws std::runtime_error For excessive count/budget, insufficient bytes or
     * fixed-shape mismatch.
     *
     * @param[in,out] value Caller-owned field: source bytes are read when writing; overwritten as
     *     decoding succeeds when reading.
     * @param[in] visit Copied callable invoked synchronously for each element; generally captures this
     *     archive and consumes element fields.
     * @param[in] fixed_size When reading, true refuses a saved size differing from current vector
     *     length; false resizes before visiting.
     */
    void sequence(std::vector<T>& value, F visit, bool fixed_size = false) {
        uint64_t size = value.size();
        field(size);
        allocate(size, sizeof(T));
        if (reading_) {
            if (size > bytes_.size() - offset_)
                fail();
            if (fixed_size && size != value.size())
                throw std::runtime_error(
                    "Checkpoint vector shape differs: saved " + std::to_string(size) + ", model " +
                    std::to_string(value.size()) + ", offset " + std::to_string(offset_) + ".");
            value.resize(static_cast<std::size_t>(size));
        }
        for (auto& element : value)
            visit(element);
    }

private:
    std::string bytes_;
    std::size_t offset_ = 0;
    bool reading_ = false;
    uint64_t allocation_remaining_ = 64 * 1024 * 1024;
    /**
     * @brief Charge a logical allocation against the remaining archive budget.
     *
     * Rejects count>remaining/width before subtracting count*width. Applies on writes as well as reads.
     *@throws std::runtime_error When the charge exceeds the remaining 64 MiB budget.
     *
     * @param[in] count Requested element count.
     * @param[in] width Positive element width in bytes; caller guarantees nonzero to avoid division by
     *     zero.
     */
    void allocate(uint64_t count, uint64_t width) {
        if (count > allocation_remaining_ / width)
            fail();
        allocation_remaining_ -= count * width;
    }
    /**
     * @brief Throw the common invalid completed-source record diagnostic.
     *
     * Always throws std::runtime_error; does not reset bytes, offset or destinations.
     */
    [[noreturn]] static void fail() {
        throw std::runtime_error("Invalid completed-source checkpoint record.");
    }
    template <class T>
    /**
     * @brief Read or append one native arithmetic/enum representation.
     *
     * Requires arithmetic or enum T. Read checks remaining bytes; bool accepts only encoded bytes zero
     * or one. Other enums are copied without domain validation. Offset advances only after this field
     * is copied.
     * @throws std::runtime_error For insufficient bytes or an invalid bool
     * representation.
     *
     * @param[in,out] value Caller-owned field: source bytes are read when writing; overwritten as
     *     decoding succeeds when reading.
     */
    void field(T& value)
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    {
        if (reading_) {
            if (sizeof(T) > bytes_.size() - offset_)
                fail();
            // Invalid bool representations must not enter a C++ object.
            if constexpr (std::is_same_v<T, bool>) {
                const auto v = static_cast<unsigned char>(bytes_[offset_]);
                if (v > 1)
                    fail();
                value = v != 0;
            } else
                std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
            offset_ += sizeof(T);
        } else
            bytes_.append(reinterpret_cast<const char*>(&value), sizeof(T));
    }
    /**
     * @brief Visit a uint64 length followed by exact owned string bytes.
     *
     * Charges length bytes to the archive budget; read checks remaining bytes before assignment.
     * Embedded NUL bytes are preserved. Allocation failures propagate and previous fields remain
     * decoded.
     *
     * @param[in,out] value Caller-owned field: source bytes are read when writing; overwritten as
     *     decoding succeeds when reading.
     */
    void field(std::string& value) {
        uint64_t size = value.size();
        field(size);
        allocate(size, 1);
        if (reading_) {
            if (size > bytes_.size() - offset_)
                fail();
            value.assign(bytes_.data() + offset_, static_cast<std::size_t>(size));
            offset_ += size;
        } else
            bytes_.append(value);
    }
    /**
     * @brief Visit a typed vector using its current shape as a read-mode requirement.
     *
     * Delegates to sequence(...,fixed_size=true), charging count*sizeof(T) and applying field() to each
     * element.
     *
     * @param[in,out] value Caller-owned field: source bytes are read when writing; overwritten as
     *     decoding succeeds when reading.
     */
    template <class T> void fixedOne(std::vector<T>& value) {
        sequence(value, [&](auto& element) { field(element); }, true);
    }
    /**
     * @brief Visit a fixed-length packed-bool vector as individual validated bool bytes.
     *
     * Saved length must equal destination size and fit remaining bytes. Uses bool temporaries to avoid
     * vector<bool> proxy references. This specialized fixed-vector path does not separately charge the
     * allocation budget.
     *
     * @param[in,out] value Caller-owned field: source bytes are read when writing; overwritten as
     *     decoding succeeds when reading.
     */
    void fixedOne(std::vector<bool>& value) {
        uint64_t size = value.size();
        field(size);
        if (reading_ && (size != value.size() || size > bytes_.size() - offset_))
            fail();
        for (std::size_t i = 0; i < value.size(); ++i) {
            bool element = value[i];
            field(element);
            if (reading_)
                value[i] = element;
        }
    }
    /**
     * @brief Visit a resizable vector through its element field encodings.
     *
     * Reads/writes uint64 count, applies logical allocation budget and visits in order; read-mode
     * resizing may invalidate references. No strong rollback guarantee.
     *
     * @param[in,out] value Caller-owned field: source bytes are read when writing; overwritten as
     *     decoding succeeds when reading.
     */
    template <class T> void field(std::vector<T>& value) {
        sequence(value, [&](auto& element) { field(element); });
    }
    /**
     * @brief Visit a resizable bool vector as uint64 length and validated bool bytes.
     *
     * Charges one byte per bool, checks remaining bytes and resizes on read. Each byte must be zero or
     * one; later failure can leave an updated prefix.
     *
     * @param[in,out] value Caller-owned field: source bytes are read when writing; overwritten as
     *     decoding succeeds when reading.
     */
    void field(std::vector<bool>& value) {
        uint64_t size = value.size();
        field(size);
        allocate(size, 1);
        if (reading_) {
            if (size > bytes_.size() - offset_)
                fail();
            value.resize(size);
        }
        for (std::size_t i = 0; i < value.size(); ++i) {
            bool element = value[i];
            field(element);
            if (reading_)
                value[i] = element;
        }
    }
    /**
     * @brief Visit a compile-time fixed array without encoding a length.
     *
     * Schema supplies N; caller must use the same extent and element type on writing/reading. Field
     * failures leave earlier elements visited.
     *
     * @param[in,out] value Caller-owned field: source bytes are read when writing; overwritten as
     *     decoding succeeds when reading.
     */
    template <class T, std::size_t N> void field(std::array<T, N>& value) {
        for (auto& element : value)
            field(element);
    }
    /**
     * @brief Visit a pair in first-then-second order without metadata.
     *
     * Schema supplies both types. Failure in second does not undo changes or consumed bytes for first.
     *
     * @param[in,out] value Caller-owned field: source bytes are read when writing; overwritten as
     *     decoding succeeds when reading.
     */
    template <class A, class B> void field(std::pair<A, B>& value) {
        field(value.first);
        field(value.second);
    }
};
}
