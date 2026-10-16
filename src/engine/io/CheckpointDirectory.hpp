// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>

namespace openswmm::io {
bool checkpointNoReplaceAvailable() noexcept;
/// Atomically publish a private file/directory without replacing any destination.
void publishCheckpointNoReplace(const std::filesystem::path& staging,const std::filesystem::path& destination);
/// Own an exclusively created private directory. Publish the complete bundle
/// without replacing an existing destination, including an empty directory.
class CheckpointDirectory {
public:
    explicit CheckpointDirectory(std::filesystem::path destination);
    ~CheckpointDirectory();
    CheckpointDirectory(const CheckpointDirectory&)=delete;
    CheckpointDirectory& operator=(const CheckpointDirectory&)=delete;
    const std::filesystem::path& staging() const {return staging_;}
    void publish();
private:
    std::filesystem::path destination_,staging_;
    bool owned_=false;
};
}
