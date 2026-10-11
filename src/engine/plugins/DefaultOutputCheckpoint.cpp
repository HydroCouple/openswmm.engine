// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "DefaultOutputPlugin.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "core/DateTime.hpp"
#include "core/FileIO.hpp"
#include "core/SimulationContext.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace openswmm {
namespace {
constexpr std::size_t maxBytes = 64 * 1024 * 1024;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string readFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    require(!ec && size <= maxBytes, "Binary report prefix is missing or exceeds its checkpoint limit.");
    std::ifstream in(path, std::ios::binary);
    std::string bytes(size, '\0');
    in.read(bytes.data(), bytes.size());
    require(bool(in) && in.peek() == std::char_traits<char>::eof(), "Cannot read binary report prefix.");
    return bytes;
}
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
    ~PreparedCheckpoint() {
        if (stream) std::fclose(stream);
        if (!staging.empty()) { std::error_code ec; std::filesystem::remove(staging, ec); }
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

void DefaultOutputPlugin::installCheckpoint(PreparedCheckpoint& prepared) {
    const auto path = io::utf8_path(out_path_); std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    const auto writable = std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write;
    require(!ec && std::filesystem::is_regular_file(status) && (status.permissions() & writable) != std::filesystem::perms::none &&
            readFile(path) == checkpoint_header_, "Binary output restore destination is no longer its fresh report header.");
    std::filesystem::permissions(prepared.staging, status.permissions(), ec);
    require(!ec, "Cannot preserve binary output destination permissions.");
    std::filesystem::rename(prepared.staging, path, ec);
    require(!ec, "Cannot publish restored binary report prefix.");
    // No throwing operation after successful publication.
    std::fclose(out_file_); out_file_ = prepared.stream; prepared.stream = nullptr;
    prepared.staging.clear(); n_periods_ = step_count_ = prepared.periods;
    state_ = n_periods_ ? PluginState::UPDATING : PluginState::PREPARED;
}
} // namespace openswmm
