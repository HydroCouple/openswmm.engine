// SPDX-License-Identifier: Apache-2.0
#include "CheckpointDirectory.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#ifdef __APPLE__
#include <fcntl.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
namespace openswmm::io {
CheckpointDirectory::CheckpointDirectory(std::filesystem::path destination)
    :destination_(std::move(destination)) {
    if(std::filesystem::exists(std::filesystem::symlink_status(destination_)))
        throw std::runtime_error("Checkpoint bundle destination already exists.");
    static std::atomic<uint64_t> serial{0};
    for(int k=0;k<32;++k) {
        staging_=destination_;staging_+=".bundle-part-"+std::to_string(++serial);
        std::error_code ec;
        if(std::filesystem::create_directory(staging_,ec)){owned_=true;return;}
        if(ec&&ec!=std::errc::file_exists)throw std::filesystem::filesystem_error("Cannot create checkpoint bundle staging directory.",staging_,ec);
    }
    throw std::runtime_error("Cannot create exclusive checkpoint bundle staging directory.");
}
CheckpointDirectory::~CheckpointDirectory() {
    if(owned_){std::error_code ec;std::filesystem::remove_all(staging_,ec);}
}
void CheckpointDirectory::publish() {
    if(!owned_)throw std::runtime_error("Checkpoint bundle staging directory is not owned.");
    // Plain filesystem::rename can replace an empty directory on POSIX.
    // Use each platform's atomic no-replace operation instead.
#ifdef _WIN32
    const bool ok=MoveFileExW(staging_.c_str(),destination_.c_str(),MOVEFILE_WRITE_THROUGH)!=0;
#elif defined(__APPLE__)
    const bool ok=renameatx_np(AT_FDCWD,staging_.c_str(),AT_FDCWD,destination_.c_str(),RENAME_EXCL)==0;
#elif defined(__linux__) && defined(SYS_renameat2)
    const bool ok=syscall(SYS_renameat2,AT_FDCWD,staging_.c_str(),AT_FDCWD,destination_.c_str(),1 /* RENAME_NOREPLACE */)==0;
#else
    const bool ok=false;
#endif
    if(!ok)throw std::runtime_error("Cannot publish checkpoint bundle without replacing its destination.");
    owned_=false;
}
}
