// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "CheckpointOutputPublication.hpp"
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#endif

namespace openswmm::io {
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
        struct _stat64 opened{},named{};
        if(_fstat64(output.original_descriptor,&opened) || _wstat64(output.destination.c_str(),&named))return false;
#else
        struct stat opened{},named{};
        if(fstat(output.original_descriptor,&opened) || stat(output.destination.c_str(),&named))return false;
#endif
        if(opened.st_dev!=named.st_dev || opened.st_ino!=named.st_ino)return false;
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
