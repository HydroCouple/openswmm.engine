// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file HotstartFormat.cpp
 * @brief Legacy HSF v4 binary hot-start file — parser + materialiser.
 *
 * @details See HotstartFormat.hpp for the scope and field-mapping rules.
 *          The binary layout mirrors `src/legacy/engine/hotstart.c`
 *          (`saveRouting`/`readRouting`) for the routing-state portion.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "HotstartFormat.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace openswmm::gpkg::formats {

namespace {

constexpr const char* HSF_STAMP_V4 = "SWMM5-HOTSTART4";
constexpr std::size_t HSF_STAMP_LEN = 15;

bool readBytes(std::FILE* fp, void* dst, std::size_t n) {
    return std::fread(dst, 1, n, fp) == n;
}
bool writeBytes(std::FILE* fp, const void* src, std::size_t n) {
    return std::fwrite(src, 1, n, fp) == n;
}

bool readInt32(std::FILE* fp, int32_t& out) {
    return readBytes(fp, &out, sizeof(out));
}
bool readFloat(std::FILE* fp, float& out) {
    return readBytes(fp, &out, sizeof(out));
}
bool writeInt32(std::FILE* fp, int32_t v) {
    return writeBytes(fp, &v, sizeof(v));
}
bool writeFloat(std::FILE* fp, float v) {
    return writeBytes(fp, &v, sizeof(v));
}

} // namespace

FormatResult parseHotstartHsf(const std::string&  path,
                               HotstartSnapshot&  snapshot) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return fail("could not open '" + path + "' for reading");

    // --- File-stamp ---
    char stamp[16] = {};
    if (!readBytes(fp, stamp, HSF_STAMP_LEN)) {
        std::fclose(fp);
        return fail("truncated header (file stamp)");
    }
    if (std::memcmp(stamp, "SWMM5-HOTSTART", 14) != 0) {
        std::fclose(fp);
        return fail("unrecognised file stamp — not a SWMM5 hot-start file");
    }
    snapshot.header.file_stamp = std::string(stamp, HSF_STAMP_LEN);
    // Version is the trailing digit; default v1 when absent (legacy).
    const char v = stamp[14];
    snapshot.header.file_version = (v >= '0' && v <= '9') ? (v - '0') : 1;

    // --- Counts (HSF v4 layout: nSubcatch, nLandUses, nNodes, nLinks,
    //                            nPollut, flowUnits) ---
    int32_t nSub = 0, nLu = 0, nNd = 0, nLk = 0, nPo = 0, fu = 0;
    if (!readInt32(fp, nSub) ||
        !readInt32(fp, nLu)  ||
        !readInt32(fp, nNd)  ||
        !readInt32(fp, nLk)  ||
        !readInt32(fp, nPo)  ||
        !readInt32(fp, fu)) {
        std::fclose(fp);
        return fail("truncated header (counts)");
    }
    snapshot.header.num_subcatch = nSub;
    snapshot.header.num_landuses = nLu;
    snapshot.header.num_nodes    = nNd;
    snapshot.header.num_links    = nLk;
    snapshot.header.num_pollut   = nPo;
    snapshot.header.flow_units   = fu;

    // --- Skip subcatch runoff state (out of scope for this slice).
    //     The runoff portion is conditional on per-subcatchment
    //     groundwater / snowpack configuration and cannot be skipped
    //     by byte-count alone. For now we refuse files that contain a
    //     non-zero subcatch count when version >= 3 — callers should
    //     prepare HSF files without runoff state, or wait for the
    //     follow-up slice.
    if (snapshot.header.file_version >= 3 && nSub > 0) {
        std::fclose(fp);
        return fail("HSF carries runoff state (nSubcatch > 0); IO-6e "
                    "supports routing state only — see HotstartFormat.hpp");
    }

    // --- Node state ---
    snapshot.node_state.clear();
    snapshot.node_state.reserve(static_cast<std::size_t>(nNd));
    for (int i = 0; i < nNd; ++i) {
        HotstartNodeState s;
        float depth = 0.0f, lat = 0.0f;
        if (!readFloat(fp, depth) || !readFloat(fp, lat)) {
            std::fclose(fp);
            return fail("truncated node state at index "
                          + std::to_string(i));
        }
        s.depth          = depth;
        s.lateral_inflow = lat;
        s.overflow       = 0.0;          // not in HSF v4
        s.concentration.resize(static_cast<std::size_t>(nPo));
        for (int j = 0; j < nPo; ++j) {
            float q = 0.0f;
            if (!readFloat(fp, q)) {
                std::fclose(fp);
                return fail("truncated node pollutant state");
            }
            s.concentration[static_cast<std::size_t>(j)] = q;
        }
        snapshot.node_state.push_back(std::move(s));
    }

    // --- Link state ---
    snapshot.link_state.clear();
    snapshot.link_state.reserve(static_cast<std::size_t>(nLk));
    for (int i = 0; i < nLk; ++i) {
        HotstartLinkState s;
        float flow = 0.0f, depth = 0.0f, setting = 0.0f;
        if (!readFloat(fp, flow)   ||
            !readFloat(fp, depth)  ||
            !readFloat(fp, setting)) {
            std::fclose(fp);
            return fail("truncated link state at index "
                          + std::to_string(i));
        }
        s.flow           = flow;
        s.depth          = depth;
        s.setting        = setting;
        s.target_setting = setting;   // legacy mirrors target=setting on read
        s.volume         = 0.0;
        s.time_open      = 0.0;
        s.time_closed    = 0.0;
        s.concentration.resize(static_cast<std::size_t>(nPo));
        for (int j = 0; j < nPo; ++j) {
            float q = 0.0f;
            if (!readFloat(fp, q)) {
                std::fclose(fp);
                return fail("truncated link pollutant state");
            }
            s.concentration[static_cast<std::size_t>(j)] = q;
        }
        snapshot.link_state.push_back(std::move(s));
    }

    std::fclose(fp);
    return ok();
}

FormatResult writeHotstartHsf(const std::string&        path,
                               const HotstartSnapshot&   snapshot) {
    const auto& h = snapshot.header;

    // Header-vs-vector consistency check.
    if (static_cast<int>(snapshot.node_state.size()) != h.num_nodes) {
        return fail("node_state size mismatches header.num_nodes");
    }
    if (static_cast<int>(snapshot.link_state.size()) != h.num_links) {
        return fail("link_state size mismatches header.num_links");
    }
    for (const auto& n : snapshot.node_state) {
        if (static_cast<int>(n.concentration.size()) != h.num_pollut) {
            return fail("node concentration array size mismatches num_pollut");
        }
    }
    for (const auto& l : snapshot.link_state) {
        if (static_cast<int>(l.concentration.size()) != h.num_pollut) {
            return fail("link concentration array size mismatches num_pollut");
        }
    }

    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return fail("could not open '" + path + "' for writing");

    // Stamp (v4).
    if (!writeBytes(fp, HSF_STAMP_V4, HSF_STAMP_LEN)) {
        std::fclose(fp);
        return fail("write failed (stamp)");
    }
    // Counts. We always write nSubcatch=0 — this slice's writer does not
    // emit runoff state; readers that load the file will skip the runoff
    // block trivially when nSubcatch==0.
    const int32_t zero = 0;
    if (!writeInt32(fp, zero) ||                              // nSubcatch
        !writeInt32(fp, h.num_landuses) ||
        !writeInt32(fp, h.num_nodes)    ||
        !writeInt32(fp, h.num_links)    ||
        !writeInt32(fp, h.num_pollut)   ||
        !writeInt32(fp, h.flow_units)) {
        std::fclose(fp);
        return fail("write failed (counts)");
    }

    // Node state.
    for (const auto& n : snapshot.node_state) {
        if (!writeFloat(fp, static_cast<float>(n.depth)) ||
            !writeFloat(fp, static_cast<float>(n.lateral_inflow))) {
            std::fclose(fp);
            return fail("write failed (node state)");
        }
        for (double q : n.concentration) {
            if (!writeFloat(fp, static_cast<float>(q))) {
                std::fclose(fp);
                return fail("write failed (node quality)");
            }
        }
    }

    // Link state.
    for (const auto& l : snapshot.link_state) {
        if (!writeFloat(fp, static_cast<float>(l.flow))    ||
            !writeFloat(fp, static_cast<float>(l.depth))   ||
            !writeFloat(fp, static_cast<float>(l.setting))) {
            std::fclose(fp);
            return fail("write failed (link state)");
        }
        for (double q : l.concentration) {
            if (!writeFloat(fp, static_cast<float>(q))) {
                std::fclose(fp);
                return fail("write failed (link quality)");
            }
        }
    }

    std::fclose(fp);
    return ok();
}

} // namespace openswmm::gpkg::formats
