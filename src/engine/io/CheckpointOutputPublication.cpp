// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "CheckpointOutputPublication.hpp"
#include "../core/FileIO.hpp"
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace openswmm::io {
FILE* checkpointOpenOutput(const std::filesystem::path& path, bool existing) {
#ifdef _WIN32
    // Publication replaces live files. CRT fopen does not share DELETE access.
    HANDLE handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,existing?OPEN_EXISTING:CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return nullptr;
    const int fd=_open_osfhandle(reinterpret_cast<intptr_t>(handle),_O_RDWR|_O_BINARY);
    if(fd<0){CloseHandle(handle);return nullptr;}
    FILE* stream=_fdopen(fd,existing?"r+b":"w+b");
    if(!stream)_close(fd);
    return stream;
#else
    return fopen_path(path,existing?"r+b":"w+bx");
#endif
}
int checkpointFileDescriptor(FILE* stream) noexcept {
#ifdef _WIN32
    return stream?_fileno(stream):-1;
#else
    return stream?fileno(stream):-1;
#endif
}
namespace {
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
bool fresh(const CheckpointOutputPublication::Output& output) {
    std::error_code ec;const auto status=std::filesystem::symlink_status(output.destination,ec);
    const auto writable=std::filesystem::perms::owner_write|std::filesystem::perms::group_write|std::filesystem::perms::others_write;
    if(ec || !std::filesystem::is_regular_file(status) || (status.permissions()&writable)==std::filesystem::perms::none ||
       std::filesystem::file_size(output.destination,ec)!=output.fresh_header.size() || ec)return false;
    if(output.original_descriptor>=0) {
#ifdef _WIN32
        // CRT stat inode fields are not Windows file identities. Compare the
        // live handle with the named file by volume serial and file index.
        const auto original=reinterpret_cast<HANDLE>(_get_osfhandle(output.original_descriptor));
        const auto named_handle=CreateFileW(output.destination.c_str(),FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(named_handle==INVALID_HANDLE_VALUE)return false;
        BY_HANDLE_FILE_INFORMATION opened{},named{};
        const bool identified=GetFileInformationByHandle(original,&opened) &&
            GetFileInformationByHandle(named_handle,&named);
        CloseHandle(named_handle);
        if(!identified || (named.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
           opened.dwVolumeSerialNumber!=named.dwVolumeSerialNumber ||
           opened.nFileIndexHigh!=named.nFileIndexHigh || opened.nFileIndexLow!=named.nFileIndexLow)return false;
#else
        struct stat opened{},named{};
        if(fstat(output.original_descriptor,&opened) || stat(output.destination.c_str(),&named))return false;
        if(opened.st_dev!=named.st_dev || opened.st_ino!=named.st_ino)return false;
#endif
    }
    std::ifstream in(output.destination,std::ios::binary);std::string bytes(output.fresh_header.size(),'\0');
    in.read(bytes.data(),bytes.size());return bool(in) && in.peek()==std::char_traits<char>::eof() && bytes==output.fresh_header;
}
}
CheckpointOutputPublication::CheckpointOutputPublication(std::vector<Output> outputs) {
    try {
        require(!outputs.empty(),"Checkpoint publication needs an output.");entries_.reserve(outputs.size());
        std::vector<std::filesystem::path> paths;
        for(auto& output:outputs) {
            require(!output.fresh_header.empty() && fresh(output),"Checkpoint destination is no longer its fresh result header.");
            require(std::filesystem::is_regular_file(std::filesystem::symlink_status(output.staging)),"Checkpoint staging file is unavailable.");
            for(const auto& path:{output.staging,output.destination}) {
                const auto canonical=std::filesystem::weakly_canonical(path);
                for(const auto& previous:paths)require(canonical!=previous && !std::filesystem::equivalent(path,previous),
                                                     "Checkpoint output paths alias each other.");
                paths.push_back(canonical);
            }
            entries_.push_back({std::move(output)});
        }
        static std::atomic<unsigned long long> serial{0};
        for(auto& entry:entries_) {
            std::error_code ec;
            std::filesystem::permissions(entry.output.staging,std::filesystem::status(entry.output.destination).permissions());
            for(int k=0;k<32;++k) {
                auto backup=entry.output.destination;backup+=".checkpoint-backup-"+std::to_string(++serial);
                std::filesystem::create_hard_link(entry.output.destination,backup,ec);
                if(!ec) {entry.backup=std::move(backup);break;}
                if(ec!=std::errc::file_exists)break;
            }
            require(!entry.backup.empty(),"Cannot reserve checkpoint output rollback header.");
        }
    } catch(...) {cleanup();throw;}
}
CheckpointOutputPublication::~CheckpointOutputPublication() {cleanup();}
void CheckpointOutputPublication::cleanup() noexcept {
    for(auto& entry:entries_)if(!entry.backup.empty() && !entry.retain) {
        std::error_code ec;std::filesystem::remove(entry.backup,ec);
    }
}
void CheckpointOutputPublication::publish() {
    for(const auto& entry:entries_) {
        require(!entry.published && fresh(entry.output),"Checkpoint destination changed before publication.");
        require(std::filesystem::equivalent(entry.output.destination,entry.backup),"Checkpoint destination identity changed before publication.");
    }
    std::error_code error;
    for(auto& entry:entries_) {
        std::filesystem::rename(entry.output.staging,entry.output.destination,error);
        if(error)break;
        entry.published=true;
    }
    if(!error)return;
    bool rollback_failed=false;
    for(auto it=entries_.rbegin();it!=entries_.rend();++it)if(it->published) {
        std::error_code ec;std::filesystem::rename(it->backup,it->output.destination,ec);
        if(ec) {it->retain=true;rollback_failed=true;} else {it->backup.clear();it->published=false;}
    }
    if(rollback_failed) {
        std::string message="Checkpoint output publication and rollback failed; recovery headers:";
        for(const auto& entry:entries_)if(entry.retain)message+=" "+entry.backup.string();
        throw std::runtime_error(message);
    }
    throw std::runtime_error("Cannot publish checkpoint outputs; fresh destinations were restored.");
}
}
