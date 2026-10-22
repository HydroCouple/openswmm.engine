// SPDX-License-Identifier: Apache-2.0
#include "CheckpointFile.hpp"
#include "CheckpointOutputPublication.hpp"
#include "../core/CompletedSourceArchive.hpp"
#include "../core/FileIO.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
namespace openswmm::io {
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
constexpr uint64_t seed=14695981039346656037ULL;
uint64_t hash(uint64_t h,const char* data,std::size_t n){for(std::size_t i=0;i<n;++i){h^=static_cast<unsigned char>(data[i]);h*=1099511628211ULL;}return h;}
struct File {FILE* f;~File(){if(f)std::fclose(f);}};
uint64_t prefixHash(FILE* f, uint64_t bytes, FILE* out = nullptr, int descriptor = -1) {
    std::array<char, 64 * 1024> buffer{};
    uint64_t h = seed, offset = 0;
    while (bytes) {
        const auto n = std::min(bytes, uint64_t(buffer.size()));
        if (descriptor >= 0)
            checkpointReadOutput(descriptor, offset, buffer.data(), n);
        else
            require(std::fread(buffer.data(), 1, n, f) == n,
                    "Checkpoint companion is truncated or unreadable.");
        offset += n;
        h = hash(h, buffer.data(), n);
        if (out)
            require(std::fwrite(buffer.data(), 1, n, out) == n,
                    "Cannot copy checkpoint companion.");
        bytes -= n;
    }
    return h;
}
}
uint64_t checkpointFileSize(const std::filesystem::path& path) {
    require(std::filesystem::is_regular_file(std::filesystem::symlink_status(path)),"Checkpoint companion must be a regular file.");
    auto bytes=std::filesystem::file_size(path);require(bytes>0 && bytes<=uint64_t(INT64_MAX),"Invalid checkpoint companion size.");return bytes;
}
void checkpointSeek(FILE* f,uint64_t offset) {
    require(f && offset<=uint64_t(INT64_MAX),"Invalid checkpoint file offset.");
#ifdef _WIN32
    require(_fseeki64(f,static_cast<int64_t>(offset),SEEK_SET)==0,"Cannot seek checkpoint file.");
#else
    require(fseeko(f,static_cast<off_t>(offset),SEEK_SET)==0,"Cannot seek checkpoint file.");
#endif
}
std::string CheckpointFile::encode() const {detail::CompletedSourceArchive a;uint32_t version=1,endian=0x01020304;auto name=path_utf8(path);auto n=bytes,h=fingerprint;a(version,endian,name,n,h);return a.bytes();}
CheckpointFile CheckpointFile::decode(std::string bytes) {
    require(bytes.size()<16384,"Checkpoint companion contract exceeds its limit.");detail::CompletedSourceArchive a(std::move(bytes));uint32_t version,endian;std::string path;CheckpointFile file;a(version,endian,path,file.bytes,file.fingerprint);a.finish();
    require(version==1 && endian==0x01020304 && !path.empty() && path.size()<8192 && path.find('\0')==std::string::npos && file.bytes>0 && file.bytes<=uint64_t(INT64_MAX),"Invalid checkpoint companion contract.");file.path=utf8_path(path);return file;
}
void CheckpointFile::verify() const {require(checkpointFileSize(path)==bytes,"Checkpoint companion size changed.");File f{fopen_path(path,"rb")};require(f.f,"Cannot open checkpoint companion.");require(prefixHash(f.f,bytes)==fingerprint,"Checkpoint companion fingerprint differs.");}
OwnedCheckpointFile::~OwnedCheckpointFile(){if(owned){std::error_code ec;std::filesystem::remove(file.path,ec);}}
std::shared_ptr<OwnedCheckpointFile> copyCheckpointFile(const std::filesystem::path& source,uint64_t bytes,const std::filesystem::path& base, int source_descriptor) {
    require(bytes>0 && bytes<=checkpointFileSize(source),"Checkpoint prefix size differs.");File in{source_descriptor < 0 ? fopen_path(source,"rb") : nullptr};require(source_descriptor >= 0 || in.f,"Cannot read checkpoint prefix.");
    auto copy=std::make_shared<OwnedCheckpointFile>();copy->owned=false;File out{nullptr};static std::atomic<uint64_t> serial{0};
    for(int k=0;k<32 && !out.f;++k){copy->file.path=base;copy->file.path+=".file-part-"+std::to_string(++serial);out.f=fopen_path(copy->file.path,"wbx");}
    require(out.f,"Cannot create exclusive checkpoint companion.");copy->owned=true;copy->file.bytes=bytes;
    copy->file.fingerprint=prefixHash(in.f,bytes,out.f,source_descriptor);require(std::fflush(out.f)==0 && !std::ferror(out.f),"Cannot flush checkpoint companion.");
#ifdef _WIN32
    require(_commit(_fileno(out.f))==0,"Cannot synchronize checkpoint companion.");
#else
    require(fsync(fileno(out.f))==0,"Cannot synchronize checkpoint companion.");
#endif
    const bool closed=std::fclose(out.f)==0;out.f=nullptr;require(closed,"Cannot close checkpoint companion.");copy->file.verify();return copy;
}
}
