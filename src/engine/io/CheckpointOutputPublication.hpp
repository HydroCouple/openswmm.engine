// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>

namespace openswmm::io {
int checkpointFileDescriptor(FILE*) noexcept;
// Create a fresh binary update stream whose handle permits checkpoint renames.
FILE* checkpointOpenOutput(const std::filesystem::path&);
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
