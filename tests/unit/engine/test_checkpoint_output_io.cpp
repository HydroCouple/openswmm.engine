// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file test_checkpoint_output_io.cpp
 * @brief Regression helpers and cases for live checkpoint output publication.
 * @author Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license Apache-2.0
 */
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <gtest/gtest.h>
#include "io/CheckpointOutputPublication.hpp"
#include "io/CheckpointFile.hpp"
#include "core/FileIO.hpp"
#include "io/CheckpointHdf5.hpp"
#include "io/Hdf5ObjectCreation.hpp"
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace openswmm;
namespace {
/**
 * @brief Create the configured checkpoint-output artifact directory and remove one destination.
 *
 * Uses throwing directory/remove operations and propagates filesystem errors. Removes only the
 * chosen path; does not create arbitrary nested parents or reserve/open the result.
 *
 * @param[in] name Safe filename/path beneath CHECKPOINT_OUTPUT_IO_DIR; caller chooses an artifact
 *     it may remove.
 * @return Owned root/name path, absent after a successful remove or already absent.
 */
std::filesystem::path path(const std::filesystem::path& name) {
    const auto root = std::filesystem::path(CHECKPOINT_OUTPUT_IO_DIR);
    std::filesystem::create_directories(root);
    auto p = root / name;
    std::filesystem::remove(p);
    return p;
}
/**
 * @brief Open a checkpoint publication output stream with automatic fclose ownership.
 *
 * Caller checks the pointer before I/O. No borrowed FILE storage survives owner destruction; opener
 * exceptions propagate, and fclose status is discarded by the deleter.
 *
 * @param[in] p Destination accepted by checkpointOpenOutput; parent exists and intended contents
 *     may be replaced.
 * @return unique_ptr<FILE,fclose> owning the opened stream, possibly null if the opener returns
 *     null.
 */
auto stream(const std::filesystem::path& p) {
    return std::unique_ptr<FILE, decltype(&std::fclose)>(io::checkpointOpenOutput(p), &std::fclose);
}
/**
 * @brief Write and flush exact bytes through a live publication-test stream.
 *
 * Fatal GoogleTest assertions require full fwrite and successful fflush. Does not close or rewind;
 * failure may leave a partial prefix and returns only from this helper.
 *
 * @param[in,out] f Required non-null writable FILE positioned where bytes should be inserted;
 *     ownership remains with caller.
 * @param[in] data Binary bytes, including embedded zeros; may be empty.
 */
void write(FILE* f, const std::string& data) {
    ASSERT_EQ(std::fwrite(data.data(), 1, data.size(), f), data.size());
    ASSERT_EQ(std::fflush(f), 0);
}
/**
 * @brief Collect an ordinary file's available binary bytes for comparisons.
 *
 * Uses unchecked stream iterators and no descriptor sharing/lifetime transfer.
 *
 * @param[in] p Input path, unchanged; concurrent locked-writer cases use liveBytes instead.
 * @return Owned available bytes; empty can mean empty/unreadable file, with no distinct
 *     partial-read status.
 */
std::string bytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
/**
 * @brief Capture a flushed HDF5 writer's current file bytes through its borrowed descriptor.
 *
 * Calls H5Fflush twice with nonfatal success expectations, allocates file_size bytes then
 * checkpointReadOutput from offset0. Requires exclusive compatible writer/read access and
 * descriptor-driver identity. Filesystem/descriptor exceptions propagate; no HDF5
 * identifier/descriptor ownership is taken.
 *
 * @param[in,out] f Valid live file created with the checkpoint HDF5 driver; global flushes mutate
 *     persistent state.
 * @param[in] p Path identifying the same live file; file_size supplies the exact read length.
 * @return Owned current physical file bytes, including any HDF5 framing/payload.
 */
std::string liveBytes(hid_t f, const std::filesystem::path& p) {
    EXPECT_GE(H5Fflush(f, H5F_SCOPE_GLOBAL), 0);
    EXPECT_GE(H5Fflush(f, H5F_SCOPE_GLOBAL), 0);
    std::string b(std::filesystem::file_size(p), '\0');
    io::checkpointReadOutput(io::checkpointHdf5Descriptor(f), 0, b.data(), b.size());
    return b;
}
/**
 * @brief Create a new scalar-value HDF5 artifact using the supplied access properties.
 *
 * Records nonfatal dataset-create/write expectations and closes temporary dataset/dataspace ids
 * with unchecked statuses. Later dataset failure can still return a nonnegative file id; caller
 * must inspect test results. No simulation output schema is created.
 *
 * @param[in] p Absent destination path; H5F_ACC_EXCL refuses an existing file.
 * @param[in] access Borrowed valid file-access property list, normally checkpointHdf5Access; not
 *     closed here.
 * @param[in] value Native integer to store in scalar dataset value with little-endian int32 file
 *     datatype.
 * @return Owned HDF5 file identifier for caller H5Fclose, or a negative create-file failure id.
 */
hid_t makeHdf(const std::filesystem::path& p, hid_t access, int value) {
    auto f = io::createHdf5File(io::path_utf8(p).c_str(), H5F_ACC_EXCL, H5P_DEFAULT, access);
    if (f < 0)
        return f;
    const auto space = H5Screate(H5S_SCALAR);
    const auto ds =
        H5Dcreate2(f, "value", H5T_STD_I32LE, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    EXPECT_GE(ds, 0);
    EXPECT_GE(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &value), 0);
    H5Dclose(ds);
    H5Sclose(space);
    return f;
}
/**
 * @brief Read the scalar integer sentinel from an open HDF5 test file.
 *
 * Uses nonfatal dataset-open/read expectations, attempts read even after open failure, closes
 * dataset and ignores close status. Does not provide a separate success flag or modify the stored
 * value.
 *
 * @param[in] f Borrowed valid file containing the numeric scalar dataset value; ownership remains
 *     with caller.
 * @return Native integer read from the dataset, initially -1 if the read does not assign it.
 */
int value(hid_t f) {
    auto ds = H5Dopen2(f, "value", H5P_DEFAULT);
    int v = -1;
    EXPECT_GE(ds, 0);
    EXPECT_GE(H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &v), 0);
    H5Dclose(ds);
    return v;
}
}
TEST(CheckpointOutputIO, DescriptorReadRestoresWriterPositionOnSuccessAndFailure) {
    const auto p = path("descriptor.bin");
    auto f = stream(p);
    ASSERT_TRUE(f);
    write(f.get(), "header");
    ASSERT_EQ(std::fseek(f.get(), 3, SEEK_SET), 0);
    const auto fd = io::checkpointFileDescriptor(f.get());
    char out[6]{};
    io::checkpointReadOutput(fd, 1, out, 3);
    EXPECT_EQ(std::string(out, 3), "ead");
    EXPECT_EQ(std::ftell(f.get()), 3);
    EXPECT_THROW(io::checkpointReadOutput(fd, 4, out, 6), std::runtime_error);
    EXPECT_EQ(std::ftell(f.get()), 3);
    EXPECT_THROW(io::checkpointReadOutput(fd, UINT64_MAX, out, 1), std::runtime_error);
}
TEST(CheckpointOutputIO, BinaryPublicationPreservesLiveOldAndNewStreams) {
    const auto p = path("binary.out"), stage = path("binary.stage");
    auto old = stream(p), next = stream(stage);
    ASSERT_TRUE(old);
    ASSERT_TRUE(next);
    write(old.get(), "header");
    write(next.get(), "prefix");
    io::CheckpointOutputPublication pub(
        {{stage, p, "header", io::checkpointFileDescriptor(old.get())}});
    pub.publish();
    EXPECT_EQ(bytes(p), "prefix");
    ASSERT_EQ(std::fseek(old.get(), 0, SEEK_SET), 0);
    char original[6]{};
    ASSERT_EQ(std::fread(original, 1, 6, old.get()), 6u);
    EXPECT_EQ(std::string(original, 6), "header");
    write(next.get(), "+continued");
    EXPECT_EQ(bytes(p), "prefix+continued");
}
TEST(CheckpointOutputIO, FailedSecondPublicationRollsBackTheFirstLiveDestination) {
    const auto a = path("rollback-a"), b = path("rollback-b"), x = path("rollback-x"),
               y = path("rollback-y");
    auto fa = stream(a), fb = stream(b), fx = stream(x);
    ASSERT_TRUE(fa);
    ASSERT_TRUE(fb);
    ASSERT_TRUE(fx);
    write(fa.get(), "A");
    write(fb.get(), "B");
    write(fx.get(), "X");
    {
        auto fy = stream(y);
        ASSERT_TRUE(fy);
        write(fy.get(), "Y");
    }
    io::CheckpointOutputPublication pub({{x, a, "A", io::checkpointFileDescriptor(fa.get())},
                                         {y, b, "B", io::checkpointFileDescriptor(fb.get())}});
#ifdef _WIN32
    // A reader that vetoes delete sharing forces a real second-rename failure.
    const auto blocker =
        CreateFileW(y.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(blocker, INVALID_HANDLE_VALUE);
    EXPECT_THROW(pub.publish(), std::runtime_error);
    CloseHandle(blocker);
#else
    std::filesystem::remove(y);
    EXPECT_THROW(pub.publish(), std::runtime_error);
#endif
    EXPECT_EQ(bytes(a), "A");
    EXPECT_EQ(bytes(b), "B");
}
TEST(CheckpointOutputIO, HdfDriverPublishesLockedOpenFilesAndKeepsThemWritable) {
    const auto p = path("mesh.h5"), stage = path("mesh.stage.h5");
    const auto access = io::checkpointHdf5Access();
    ASSERT_GE(access, 0);
    auto old = makeHdf(p, access, 0), next = makeHdf(stage, access, 17);
    H5Pclose(access);
    ASSERT_GE(old, 0);
    ASSERT_GE(next, 0);
    const auto header = liveBytes(old, p);
    liveBytes(next, stage);
#ifdef _WIN32
    // HDF5's exclusive byte lock rejects a second handle's read even though
    // delete sharing is enabled. Borrowed reads must still capture this prefix.
    std::ifstream external(p, std::ios::binary);
    char c{};
    external.read(&c, 1);
    EXPECT_FALSE(external);
    external.close();
#endif
    io::CheckpointOutputPublication pub({{stage, p, header, io::checkpointHdf5Descriptor(old)}});
    pub.publish();
    EXPECT_EQ(value(old), 0);
    EXPECT_EQ(value(next), 17);
    auto ds = H5Dopen2(next, "value", H5P_DEFAULT);
    int updated = 18;
    ASSERT_GE(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &updated), 0);
    H5Dclose(ds);
    EXPECT_GE(H5Fclose(old), 0);
    EXPECT_GE(H5Fclose(next), 0);
    const auto ordinary = H5Fopen(io::path_utf8(p).c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    ASSERT_GE(ordinary, 0);
    EXPECT_EQ(value(ordinary), 18);
    EXPECT_GE(H5Fclose(ordinary), 0);
}
TEST(CheckpointOutputIO, HdfCompanionCopiesThroughTheLockedWriterDescriptor) {
    const auto p = path("companion.h5"), base = path("companion-copy");
    const auto access = io::checkpointHdf5Access();
    ASSERT_GE(access, 0);
    const auto file = makeHdf(p, access, 33);
    H5Pclose(access);
    ASSERT_GE(file, 0);
    const auto prefix = liveBytes(file, p);
    const auto copy =
        io::copyCheckpointFile(p, prefix.size(), base, io::checkpointHdf5Descriptor(file));
    EXPECT_EQ(bytes(copy->file.path), prefix);
    EXPECT_NO_THROW(copy->file.verify());
    EXPECT_EQ(value(file), 33);
    H5Fclose(file);
}
TEST(CheckpointOutputIO, HdfPublicationRejectsAReplacedDestinationIdentity) {
    const auto p = path("identity.h5"), stage = path("identity.stage.h5"),
               moved = path("identity.moved.h5");
    const auto access = io::checkpointHdf5Access();
    ASSERT_GE(access, 0);
    auto old = makeHdf(p, access, 0), next = makeHdf(stage, access, 9);
    H5Pclose(access);
    ASSERT_GE(old, 0);
    ASSERT_GE(next, 0);
    const auto header = liveBytes(old, p);
    liveBytes(next, stage);
    std::filesystem::rename(p, moved);
    {
        std::ofstream replacement(p, std::ios::binary);
        replacement.write(header.data(), header.size());
    }
    EXPECT_THROW(
        (io::CheckpointOutputPublication({{stage, p, header, io::checkpointHdf5Descriptor(old)}})),
        std::runtime_error);
    EXPECT_EQ(bytes(p), header);
    EXPECT_EQ(value(old), 0);
    EXPECT_EQ(value(next), 9);
    H5Fclose(old);
    H5Fclose(next);
}

TEST(CheckpointOutputIO, HdfDriverAcceptsUnicodeOutputPaths) {
    const auto p = path(std::filesystem::path(u8"results-\u00e9-\u96ea.h5"));
    const auto access = io::checkpointHdf5Access();
    ASSERT_GE(access, 0);
    const auto file = makeHdf(p, access, 42);
    H5Pclose(access);
    ASSERT_GE(file, 0);
    EXPECT_FALSE(liveBytes(file, p).empty());
    EXPECT_GE(H5Fclose(file), 0);
    const auto ordinary = H5Fopen(io::path_utf8(p).c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    ASSERT_GE(ordinary, 0);
    EXPECT_EQ(value(ordinary), 42);
    H5Fclose(ordinary);
}

TEST(CheckpointOutputIO, HdfDriverReopensALiveWriterWithoutTakingASecondLock) {
    const auto p = path("live-reader.h5");
    const auto access = io::checkpointHdf5Access();
    ASSERT_GE(access, 0);
    const auto writer = makeHdf(p, access, 29);
    ASSERT_GE(writer, 0);
    EXPECT_GE(H5Fflush(writer, H5F_SCOPE_GLOBAL), 0);
    const auto reader = H5Fopen(io::path_utf8(p).c_str(), H5F_ACC_RDONLY, access);
    H5Pclose(access);
    ASSERT_GE(reader, 0);
    EXPECT_EQ(value(reader), 29);
    EXPECT_GE(H5Fclose(reader), 0);
    EXPECT_EQ(value(writer), 29);
    EXPECT_GE(H5Fclose(writer), 0);
}
