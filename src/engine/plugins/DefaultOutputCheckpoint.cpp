// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "DefaultOutputPlugin.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "core/DateTime.hpp"
#include "core/FileIO.hpp"
#include "core/SimulationContext.hpp"
#include <atomic>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace openswmm {
namespace {
constexpr std::size_t maxBytes = 64 * 1024 * 1024;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class T> T scalar(const std::string& bytes, std::size_t offset) {
    T value; std::memcpy(&value, bytes.data() + offset, sizeof(T)); return value;
}
}

// Keep the staged stream open through publication: no throwing reopen follows
// installation of solver state. On platforms that cannot rename an open file,
// publication fails before any solver state is installed.
struct DefaultOutputPlugin::PreparedCheckpoint {
    std::filesystem::path staging;
    FILE* stream = nullptr;
    int periods = 0;
    std::shared_ptr<io::OwnedCheckpointFile> file_copy;
    ~PreparedCheckpoint() {
        if (stream) std::fclose(stream);
        if (!file_copy && !staging.empty()) { std::error_code ec; std::filesystem::remove(staging, ec); }
    }
};

std::string DefaultOutputPlugin::readPrefix() const {
    require(out_file_ && !std::ferror(out_file_) && std::fflush(out_file_) == 0,
            "Cannot flush binary report prefix.");
    const long position = std::ftell(out_file_);
    require(position >= 0 && static_cast<unsigned long>(position) <= maxBytes,
            "Binary report prefix exceeds its checkpoint limit.");
    std::string bytes(position, '\0');
    require(std::fseek(out_file_, 0, SEEK_SET) == 0, "Cannot seek binary report prefix.");
    const bool read = std::fread(bytes.data(), 1, bytes.size(), out_file_) == bytes.size();
    const bool restored = std::fseek(out_file_, position, SEEK_SET) == 0;
    require(read && restored && !std::ferror(out_file_), "Cannot read binary report prefix.");
    return bytes;
}

void DefaultOutputPlugin::validateCheckpoint(const SimulationContext& ctx, double next_report_ms,
                                             int periods, const std::string& bytes) const {
    require(checkpoint_fresh_ && out_file_ && !checkpoint_header_.empty() &&
            output_start_pos_ == static_cast<long>(checkpoint_header_.size()),
            "Binary output checkpoint writer is not prepared.");
    require(periods >= 0 && bytes.size() <= maxBytes && bytes.starts_with(checkpoint_header_),
            "Binary output checkpoint header differs.");
    const std::size_t values = std::size_t(n_subcatch_) * n_subcatch_vars_ +
        std::size_t(n_nodes_) * n_node_vars_ + std::size_t(n_links_) * n_link_vars_ + MAX_SYS_RESULTS;
    const std::size_t width = sizeof(double) + values * sizeof(float);
    require(std::size_t(periods) <= (maxBytes - checkpoint_header_.size()) / width &&
            bytes.size() == checkpoint_header_.size() + std::size_t(periods) * width,
            "Binary output checkpoint report shape differs.");
    const double step_ms = 1000.0 * ctx.options.report_step;
    require(std::isfinite(step_ms) && step_ms > 0 && std::isfinite(next_report_ms) && next_report_ms >= step_ms,
            "Invalid binary report clock.");
    const double grid = std::round(next_report_ms / step_ms);
    require(grid <= std::numeric_limits<int>::max() && grid * step_ms == next_report_ms,
            "Binary output checkpoint report clock is off its grid.");
    const int completed = static_cast<int>(grid) - 1;
    const auto date = [&](int report) { return datetime::addSeconds(ctx.options.start_date, (report * step_ms + 1.0) / 1000.0); };
    // REPORT_START can hide early grid points. Compare the actual writer dates
    // rather than rounding a floating calendar-date difference at an endpoint.
    int first = 1, upper = completed + 1;
    while (first < upper) {
        const int middle = first + (upper - first) / 2;
        if (date(middle) < ctx.options.report_start) first = middle + 1;
        else upper = middle;
    }
    require(periods == completed - first + 1, "Binary report count differs from the restored report clock.");
    for (int p = 0; p < periods; ++p) {
        const auto offset = checkpoint_header_.size() + std::size_t(p) * width;
        require(scalar<double>(bytes, offset) == date(first + p), "Binary output checkpoint report date differs.");
        for (std::size_t v = 0; v < values; ++v)
            require(std::isfinite(scalar<float>(bytes, offset + sizeof(double) + v * sizeof(float))),
                    "Nonfinite binary output checkpoint value.");
    }
}

std::string DefaultOutputPlugin::checkpointBytes(const SimulationContext& ctx, double next_report_ms) const {
    auto prefix = readPrefix();
    require(step_count_ == n_periods_, "Binary output checkpoint writer counters differ.");
    validateCheckpoint(ctx, next_report_ms, n_periods_, prefix);
    detail::CompletedSourceArchive out; uint32_t version = 1; int periods = n_periods_;
    out(version, periods, prefix); return out.bytes();
}

std::shared_ptr<DefaultOutputPlugin::PreparedCheckpoint> DefaultOutputPlugin::prepareCheckpoint(
    const SimulationContext& ctx, double next_report_ms, std::string bytes) {
    require(n_periods_ == 0 && step_count_ == 0 && readPrefix() == checkpoint_header_,
            "Binary output restore requires a fresh writer.");
    detail::CompletedSourceArchive in(std::move(bytes)); uint32_t version = 0; int periods = 0; std::string prefix;
    in(version, periods, prefix); in.finish();
    require(version == 1, "Unsupported binary output checkpoint version.");
    validateCheckpoint(ctx, next_report_ms, periods, prefix);
    auto prepared = std::make_shared<PreparedCheckpoint>(); prepared->periods = periods;
    const auto path = io::utf8_path(out_path_);
    static std::atomic<unsigned long long> serial{0};
    for (int i = 0; i < 32 && !prepared->stream; ++i) {
        prepared->staging = path;
        prepared->staging += ".report-part-" + std::to_string(++serial);
        prepared->stream = io::fopen_path(prepared->staging, "w+bx");
        if (!prepared->stream) prepared->staging.clear(); // never discard somebody else's file
    }
    require(prepared->stream, "Cannot create staged binary report prefix.");
    std::setvbuf(prepared->stream, nullptr, _IOFBF, 1u << 20);
    require(std::fwrite(prefix.data(), 1, prefix.size(), prepared->stream) == prefix.size() &&
            std::fflush(prepared->stream) == 0 && !std::ferror(prepared->stream),
            "Cannot stage binary report prefix.");
    return prepared;
}

io::CheckpointOutputPublication::Output DefaultOutputPlugin::checkpointPublication(const PreparedCheckpoint& prepared) const {
    return {prepared.staging,io::utf8_path(out_path_),checkpoint_header_,io::checkpointFileDescriptor(out_file_)};
}
void DefaultOutputPlugin::installCheckpoint(PreparedCheckpoint& prepared) {
    io::CheckpointOutputPublication publication({checkpointPublication(prepared)});
    publication.publish();adoptCheckpoint(prepared);
}
void DefaultOutputPlugin::adoptCheckpoint(PreparedCheckpoint& prepared) noexcept {
    // No throwing operation after successful publication.
    std::fclose(out_file_); out_file_ = prepared.stream; prepared.stream = nullptr;
    if(prepared.file_copy)prepared.file_copy->retain();
    prepared.staging.clear(); n_periods_ = step_count_ = prepared.periods;
    state_ = n_periods_ ? PluginState::UPDATING : PluginState::PREPARED;
}

void DefaultOutputPlugin::validateFileCheckpoint(FILE* file,uint64_t bytes,const SimulationContext& ctx,double next,int periods) const {
    require(checkpoint_fresh_ && file && !checkpoint_header_.empty() && periods>=0,"Binary file checkpoint writer is not prepared.");
    const uint64_t values=uint64_t(n_subcatch_)*n_subcatch_vars_+uint64_t(n_nodes_)*n_node_vars_+uint64_t(n_links_)*n_link_vars_+MAX_SYS_RESULTS;
    const uint64_t width=sizeof(double)+values*sizeof(float);
    require(uint64_t(periods)<=(UINT64_MAX-checkpoint_header_.size())/width && bytes==checkpoint_header_.size()+uint64_t(periods)*width,"Binary file checkpoint shape differs.");
    const double step=1000*ctx.options.report_step,grid=std::round(next/step);
    require(std::isfinite(step)&&step>0&&std::isfinite(grid)&&grid>=1&&grid<=std::numeric_limits<int>::max()&&grid*step==next,"Binary file checkpoint clock differs.");
    const int completed=int(grid)-1;const auto date=[&](int k){return datetime::addSeconds(ctx.options.start_date,(k*step+1)/1000);};
    int first=1,upper=completed+1;while(first<upper){const int mid=first+(upper-first)/2;if(date(mid)<ctx.options.report_start)first=mid+1;else upper=mid;}
    require(periods==completed-first+1,"Binary file checkpoint report count differs.");
    io::checkpointSeek(file,0);std::string header(checkpoint_header_.size(),'\0');
    require(std::fread(header.data(),1,header.size(),file)==header.size()&&header==checkpoint_header_,"Binary file checkpoint header differs.");
    std::array<float,16384> buffer{};
    for(int p=0;p<periods;++p){double time;require(std::fread(&time,sizeof(time),1,file)==1&&time==date(first+p),"Binary file checkpoint date differs.");
        uint64_t left=values;while(left){const auto n=std::min(left,uint64_t(buffer.size()));require(std::fread(buffer.data(),sizeof(float),n,file)==n,"Binary file checkpoint is truncated.");for(uint64_t i=0;i<n;++i)require(std::isfinite(buffer[i]),"Nonfinite binary file checkpoint value.");left-=n;}}
    require(std::fgetc(file)==EOF&&!std::ferror(file),"Binary file checkpoint contains extra data.");
}
std::string DefaultOutputPlugin::captureFileCheckpoint(const SimulationContext& ctx,double next,const std::filesystem::path& base,
    std::shared_ptr<io::OwnedCheckpointFile>& artifact) const {
    require(out_file_ && std::fflush(out_file_)==0 && !std::ferror(out_file_) && step_count_==n_periods_,"Cannot settle binary file checkpoint.");
    auto copy=io::copyCheckpointFile(io::utf8_path(out_path_),io::checkpointFileSize(io::utf8_path(out_path_)),base);
    FILE* f=io::fopen_path(copy->file.path,"rb");require(f,"Cannot validate binary companion.");
    try{validateFileCheckpoint(f,copy->file.bytes,ctx,next,n_periods_);}catch(...){std::fclose(f);throw;}std::fclose(f);
    detail::CompletedSourceArchive a;uint32_t version=1;auto periods=n_periods_;auto file=copy->file.encode();a(version,periods,file);artifact=std::move(copy);return a.bytes();
}
std::shared_ptr<DefaultOutputPlugin::PreparedCheckpoint> DefaultOutputPlugin::prepareFileCheckpoint(const SimulationContext& ctx,double next,std::string bytes) {
    require(n_periods_==0&&step_count_==0&&readPrefix()==checkpoint_header_,"Binary file restore requires its fresh writer.");
    detail::CompletedSourceArchive a(std::move(bytes));uint32_t version;int periods;std::string descriptor;a(version,periods,descriptor);a.finish();require(version==1,"Binary file checkpoint version differs.");
    auto saved=io::CheckpointFile::decode(std::move(descriptor));saved.verify();
    auto prepared=std::make_shared<PreparedCheckpoint>();prepared->periods=periods;prepared->file_copy=io::copyCheckpointFile(saved.path,saved.bytes,io::utf8_path(out_path_));
    require(prepared->file_copy->file.fingerprint==saved.fingerprint,"Binary companion changed during copying.");
    prepared->staging=prepared->file_copy->file.path;prepared->stream=io::fopen_path(prepared->staging,"r+b");require(prepared->stream,"Cannot open staged binary companion.");
    validateFileCheckpoint(prepared->stream,saved.bytes,ctx,next,periods);io::checkpointSeek(prepared->stream,saved.bytes);return prepared;
}
} // namespace openswmm
