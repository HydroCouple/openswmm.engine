// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace openswmm::io {
int checkpointFileDescriptor(FILE*) noexcept;
/// Read an exact range through a borrowed, seekable writer descriptor, restoring
/// its position on success or failure. Required for Windows mandatory HDF5 locks.
/// Call only while the writer is flushed and exclusively held by the caller.
void checkpointReadOutput(int descriptor, uint64_t offset, char* data, std::size_t bytes);
// Create exclusively, or reopen without truncation, with rename sharing.
FILE* checkpointOpenOutput(const std::filesystem::path&, bool existing = false);
// Internal checkpoint publication. Writers remain on their old handles until
// every rename succeeds. This is not an atomic view for external readers or a
// power-loss transaction. A failed rollback retains its backup for recovery.
class CheckpointOutputPublication {
public:
    struct Output {
        std::filesystem::path staging, destination;
        std::string fresh_header;
        int original_descriptor = -1;
    };
    explicit CheckpointOutputPublication(std::vector<Output>);
    ~CheckpointOutputPublication();
    CheckpointOutputPublication(const CheckpointOutputPublication&) = delete;
    CheckpointOutputPublication& operator=(const CheckpointOutputPublication&) = delete;
    void publish();
private:
    struct Entry { Output output; std::filesystem::path backup; bool published=false, retain=false; };
    std::vector<Entry> entries_;
    void cleanup() noexcept;
};
}
