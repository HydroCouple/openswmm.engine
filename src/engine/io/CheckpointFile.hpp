// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <cstdint>
#include <cstdio>

namespace openswmm::io {
/// Internal immutable output companion. Integrity detects accidental damage,
/// not malicious edits. Physical/model archives remain independently validated.
struct CheckpointFile {
    std::filesystem::path path;
    uint64_t bytes=0,fingerprint=0;
    std::string encode() const;
    static CheckpointFile decode(std::string);
    void verify() const; // fixed 64 KiB read buffer
};
struct OwnedCheckpointFile {
    CheckpointFile file;
    bool owned=true;
    ~OwnedCheckpointFile();
    void retain() noexcept {owned=false;}
};
/// Copy exactly a known prefix; validate the copy again before publication.
/// Exclusive creation and ownership ensure failures cannot remove other files.
/// A supplied descriptor is borrowed from a flushed, exclusively held writer;
/// its position is preserved, avoiding a second handle under mandatory locks.
std::shared_ptr<OwnedCheckpointFile> copyCheckpointFile(const std::filesystem::path& source,
    uint64_t bytes,const std::filesystem::path& staging_base, int source_descriptor = -1);
uint64_t checkpointFileSize(const std::filesystem::path&);
void checkpointSeek(FILE*,uint64_t);
}
