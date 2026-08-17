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
 * @file HotStartManager.cpp
 * @brief Hot start file I/O — implementation.
 *
 * @details Implements the OPENSWMM_HS_V1 binary format:
 *
 *          ```
 *          [magic 16B][version 4B][timestamp 8B][sim_time 8B]
 *          [start_date 8B][end_date 8B][crs_len 4B][crs nB]
 *          [node_count 4B] { [name_len 4B][name nB][depth 8B][head 8B][volume 8B] } ...
 *          [link_count 4B] { [name_len 4B][name nB][flow 8B][depth 8B][volume 8B] } ...
 *          [subcatch_count 4B] { [name_len 4B][name nB][runoff 8B][gwater 8B] } ...
 *          [crc32 4B]
 *          ```
 *
 *          Strings are stored as: uint32_t length (including NUL) followed by
 *          the null-terminated bytes. Empty CRS is stored as length=1, '\0'.
 *
 * @see HotStartManager.hpp
 * @see Legacy reference: src/solver/hotstart.c
 * @ingroup engine_hotstart
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "HotStartManager.hpp"
#include "SimulationContext.hpp"
#include "../hydrology/Runoff.hpp"
#include "../hydrology/Groundwater.hpp"

#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace openswmm {

// ============================================================================
// Thread-local last error
// ============================================================================

static thread_local std::string tl_last_io_error;

const std::string& HotStartManager::last_io_error() noexcept {
    return tl_last_io_error;
}

// ============================================================================
// CRC32 (IEEE 802.3) — no external dependency
// ============================================================================

static uint32_t compute_crc32_table(uint32_t i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k)
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    return c;
}

uint32_t HotStartManager::crc32(const uint8_t* data, std::size_t len) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        const uint8_t idx = static_cast<uint8_t>((crc ^ b) & 0xFFu);
        // Inline table lookup to avoid static-init-order issues
        crc = (crc >> 8) ^ compute_crc32_table(idx);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ============================================================================
// Binary I/O helpers
// ============================================================================

namespace {

/** Write a POD value as little-endian bytes. */
template<typename T>
static bool write_pod(std::ostream& os, const T& v) {
    os.write(reinterpret_cast<const char*>(&v), sizeof(v));
    return os.good();
}

/** Write a length-prefixed string (uint32 length including NUL + bytes). */
static bool write_string(std::ostream& os, const std::string& s) {
    const uint32_t len = static_cast<uint32_t>(s.size() + 1u); // +1 for NUL
    if (!write_pod(os, len)) return false;
    os.write(s.c_str(), len);
    return os.good();
}

/** Read a POD value. */
template<typename T>
static bool read_pod(std::istream& is, T& v) {
    is.read(reinterpret_cast<char*>(&v), sizeof(v));
    return is.good();
}

/** Read a length-prefixed string. */
static bool read_string(std::istream& is, std::string& s) {
    uint32_t len = 0;
    if (!read_pod(is, len)) return false;
    if (len == 0 || len > 4096u) return false; // sanity guard
    std::vector<char> buf(len);
    is.read(buf.data(), len);
    if (!is.good()) return false;
    // Remove trailing NUL (stored as part of the string)
    s.assign(buf.data(), buf.data() + (len - 1u));
    return true;
}

} // anonymous namespace

// ============================================================================
// write_file()
// ============================================================================

bool HotStartManager::write_file(const HotStartFile& hs, const std::string& path) {
    // Accumulate into a memory buffer first so we can compute CRC on it
    std::ostringstream buf(std::ios::binary);

    // Magic (16 bytes — includes NUL)
    static constexpr char MAGIC[16] = "OPENSWMM_HS_V1";
    buf.write(MAGIC, 16);

    // Header fields
    if (!write_pod(buf, hs.header.version))    return false;
    if (!write_pod(buf, hs.header.timestamp))  return false;
    if (!write_pod(buf, hs.header.sim_time))   return false;
    if (!write_pod(buf, hs.header.start_date)) return false;
    if (!write_pod(buf, hs.header.end_date))   return false;
    if (!write_string(buf, hs.header.crs))     return false;

    // Nodes
    const auto node_count = static_cast<uint32_t>(hs.nodes.size());
    if (!write_pod(buf, node_count)) return false;
    for (const auto& n : hs.nodes) {
        if (!write_string(buf, n.id))    return false;
        if (!write_pod(buf, n.depth))    return false;
        if (!write_pod(buf, n.head))     return false;
        if (!write_pod(buf, n.volume))   return false;
        // V3 (A2a): water age, -1 = not tracked.
        if (hs.header.version >= 3u) {
            if (!write_pod(buf, n.age))  return false;
        }
    }

    // Links
    const auto link_count = static_cast<uint32_t>(hs.links.size());
    if (!write_pod(buf, link_count)) return false;
    for (const auto& l : hs.links) {
        if (!write_string(buf, l.id))    return false;
        if (!write_pod(buf, l.flow))     return false;
        if (!write_pod(buf, l.depth))    return false;
        if (!write_pod(buf, l.volume))   return false;
        // V3 (A2a): water age, -1 = not tracked.
        if (hs.header.version >= 3u) {
            if (!write_pod(buf, l.age))  return false;
        }
    }

    // Subcatchments
    const auto sub_count = static_cast<uint32_t>(hs.subcatches.size());
    if (!write_pod(buf, sub_count)) return false;
    for (const auto& s : hs.subcatches) {
        if (!write_string(buf, s.id))    return false;
        if (!write_pod(buf, s.runoff))   return false;
        if (!write_pod(buf, s.gwater))   return false;
        // V2: infiltration model state + GW zone state
        if (hs.header.version >= 2u) {
            uint32_t im = static_cast<uint32_t>(s.infil_model < 0 ? 0 : s.infil_model);
            if (!write_pod(buf, im)) return false;
            for (int k = 0; k < 6; ++k) {
                if (!write_pod(buf, s.infil[k])) return false;
            }
            if (!write_pod(buf, s.gw_theta))       return false;
            if (!write_pod(buf, s.gw_lower_depth)) return false;
        }
    }

    // Compute CRC32 over the body
    const std::string body = buf.str();
    const uint32_t crc = crc32(
        reinterpret_cast<const uint8_t*>(body.data()),
        body.size()
    );

    // Write body + checksum to actual file
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        tl_last_io_error = "Cannot open '" + path + "' for writing";
        return false;
    }
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!write_pod(file, crc)) {
        tl_last_io_error = "Write error on '" + path + "'";
        return false;
    }
    return file.good();
}

// ============================================================================
// read_file()
// ============================================================================

bool HotStartManager::read_file(HotStartFile& hs, const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        tl_last_io_error = "Cannot open '" + path + "' for reading";
        return false;
    }

    // Read entire file into memory for CRC validation
    file.seekg(0, std::ios::end);
    const auto file_size = static_cast<std::size_t>(file.tellg());
    if (file_size < 16u + 4u + 4u) { // magic + version + crc minimum
        tl_last_io_error = "File '" + path + "' is too small to be a valid hot start file";
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> raw(file_size);
    file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(file_size));
    if (!file) {
        tl_last_io_error = "Read error on '" + path + "'";
        return false;
    }

    // Validate CRC32 (last 4 bytes)
    const uint32_t stored_crc = *reinterpret_cast<const uint32_t*>(raw.data() + file_size - 4);
    const uint32_t computed_crc = crc32(raw.data(), file_size - 4);
    if (stored_crc != computed_crc) {
        tl_last_io_error = "CRC32 checksum mismatch in '" + path
                           + "' (corrupt or truncated file)";
        return false;
    }

    // Parse — wrap in a stream over the raw bytes (excluding trailing CRC)
    std::string body(reinterpret_cast<const char*>(raw.data()), file_size - 4);
    std::istringstream is(body, std::ios::binary);

    // Magic
    char magic[16] = {};
    is.read(magic, 16);
    if (!is || std::memcmp(magic, "OPENSWMM_HS_V1", 15) != 0) {
        tl_last_io_error = "Invalid magic number in '" + path + "'";
        return false;
    }

    // Header
    if (!read_pod(is, hs.header.version))    return false;
    if (hs.header.version < 1u || hs.header.version > 3u) {
        tl_last_io_error = "Unsupported hot start version " +
                           std::to_string(hs.header.version) + " in '" + path + "'";
        return false;
    }
    if (!read_pod(is, hs.header.timestamp))  return false;
    if (!read_pod(is, hs.header.sim_time))   return false;
    if (!read_pod(is, hs.header.start_date)) return false;
    if (!read_pod(is, hs.header.end_date))   return false;
    if (!read_string(is, hs.header.crs))     return false;

    // Nodes
    uint32_t node_count = 0;
    if (!read_pod(is, node_count)) return false;
    hs.nodes.resize(node_count);
    for (auto& n : hs.nodes) {
        if (!read_string(is, n.id))  return false;
        if (!read_pod(is, n.depth))  return false;
        if (!read_pod(is, n.head))   return false;
        if (!read_pod(is, n.volume)) return false;
        // V3 (A2a): water age; pre-V3 files leave the -1 default.
        if (hs.header.version >= 3u) {
            if (!read_pod(is, n.age)) return false;
        }
    }

    // Links
    uint32_t link_count = 0;
    if (!read_pod(is, link_count)) return false;
    hs.links.resize(link_count);
    for (auto& l : hs.links) {
        if (!read_string(is, l.id))  return false;
        if (!read_pod(is, l.flow))   return false;
        if (!read_pod(is, l.depth))  return false;
        if (!read_pod(is, l.volume)) return false;
        // V3 (A2a): water age; pre-V3 files leave the -1 default.
        if (hs.header.version >= 3u) {
            if (!read_pod(is, l.age)) return false;
        }
    }

    // Subcatchments
    uint32_t sub_count = 0;
    if (!read_pod(is, sub_count)) return false;
    hs.subcatches.resize(sub_count);
    for (auto& s : hs.subcatches) {
        if (!read_string(is, s.id))    return false;
        if (!read_pod(is, s.runoff))   return false;
        if (!read_pod(is, s.gwater))   return false;
        // V2: infiltration model state + GW zone state
        if (hs.header.version >= 2u) {
            uint32_t im = 0;
            if (!read_pod(is, im)) return false;
            s.infil_model = static_cast<int>(im);
            for (int k = 0; k < 6; ++k) {
                if (!read_pod(is, s.infil[k])) return false;
            }
            if (!read_pod(is, s.gw_theta))       return false;
            if (!read_pod(is, s.gw_lower_depth)) return false;
        }
    }

    hs.path = path;
    return true;
}

// ============================================================================
// HotStartFile modification helpers
// ============================================================================

bool HotStartFile::set_node_depth(const std::string& id, double v) {
    for (auto& n : nodes) {
        if (n.id == id) { n.depth = v; dirty = true; return true; }
    }
    return false;
}

bool HotStartFile::set_node_head(const std::string& id, double v) {
    for (auto& n : nodes) {
        if (n.id == id) { n.head = v; dirty = true; return true; }
    }
    return false;
}

bool HotStartFile::set_link_flow(const std::string& id, double v) {
    for (auto& l : links) {
        if (l.id == id) { l.flow = v; dirty = true; return true; }
    }
    return false;
}

bool HotStartFile::set_link_depth(const std::string& id, double v) {
    for (auto& l : links) {
        if (l.id == id) { l.depth = v; dirty = true; return true; }
    }
    return false;
}

bool HotStartFile::set_subcatch_runoff(const std::string& id, double v) {
    for (auto& s : subcatches) {
        if (s.id == id) { s.runoff = v; dirty = true; return true; }
    }
    return false;
}

// ============================================================================
// HotStartManager::save()
// ============================================================================

HotStartFile* HotStartManager::save(const SimulationContext& ctx,
                                    const std::string& path) {
    tl_last_io_error.clear();

    // Auto-promote to V2 when ctx exposes solver-internal state via
    // accessors, and to V3 when water age is tracked (A2a — the age field
    // rides every node/link record; V3 implies the V2 subcatch fields,
    // written as defaults when no accessors are wired).
    const bool use_v2 = ctx.state_accessors.can_read();

    auto* hs = new HotStartFile();
    hs->path = path;

    // Header
    hs->header.version    = ctx.options.water_age ? 3u : (use_v2 ? 2u : 1u);
    hs->header.timestamp  = static_cast<int64_t>(std::time(nullptr));
    hs->header.sim_time   = ctx.current_time;
    hs->header.start_date = ctx.options.start_date;
    hs->header.end_date   = ctx.options.end_date;
    hs->header.crs        = ctx.spatial.crs;

    // Nodes — capture depth/head/volume from live SoA arrays
    const int n_nodes = ctx.n_nodes();
    hs->nodes.resize(static_cast<std::size_t>(n_nodes));
    for (int i = 0; i < n_nodes; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        hs->nodes[ui].id     = ctx.node_names.name_of(i);
        hs->nodes[ui].depth  = ctx.nodes.depth[ui];
        hs->nodes[ui].head   = ctx.nodes.head[ui];
        hs->nodes[ui].volume = ctx.nodes.volume[ui];
        if (ctx.options.water_age &&
            ui < ctx.water_age_state.node_age.size())
            hs->nodes[ui].age = ctx.water_age_state.node_age[ui];
    }

    // Links
    const int n_links = ctx.n_links();
    hs->links.resize(static_cast<std::size_t>(n_links));
    for (int i = 0; i < n_links; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        hs->links[ui].id     = ctx.link_names.name_of(i);
        hs->links[ui].flow   = ctx.links.flow[ui];
        hs->links[ui].depth  = ctx.links.depth[ui];
        hs->links[ui].volume = ctx.links.volume[ui];
        if (ctx.options.water_age &&
            ui < ctx.water_age_state.link_age.size())
            hs->links[ui].age = ctx.water_age_state.link_age[ui];
    }

    // Subcatchments — V2 includes infil + GW state pulled via accessors
    const int n_sub = ctx.n_subcatches();
    hs->subcatches.resize(static_cast<std::size_t>(n_sub));
    for (int i = 0; i < n_sub; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        auto& rec = hs->subcatches[ui];
        rec.id     = ctx.subcatch_names.name_of(i);
        rec.runoff = ctx.subcatches.runoff[ui];
        rec.gwater = ctx.subcatches.gw_flow[ui];

        if (use_v2) {
            if (ctx.state_accessors.get_infil_state) {
                ctx.state_accessors.get_infil_state(i, rec.infil_model, rec.infil);
            } else {
                rec.infil_model = 0;
            }
            if (ctx.state_accessors.get_gw_state) {
                ctx.state_accessors.get_gw_state(i, rec.gw_theta, rec.gw_lower_depth);
            }
        }
    }

    if (!write_file(*hs, path)) {
        delete hs;
        return nullptr;
    }

    return hs;
}

// ============================================================================
// HotStartManager::save() — V2 overload with infil + GW state (Gap #54)
// ============================================================================

HotStartFile* HotStartManager::save(const SimulationContext& ctx,
                                    const runoff::RunoffSolver* runoff_solver,
                                    const groundwater::GWSolver* gw_solver,
                                    const std::string& path) {
    tl_last_io_error.clear();

    auto* hs = new HotStartFile();
    hs->path = path;

    // Header — V2
    hs->header.version    = 2;
    hs->header.timestamp  = static_cast<int64_t>(std::time(nullptr));
    hs->header.sim_time   = ctx.current_time;
    hs->header.start_date = ctx.options.start_date;
    hs->header.end_date   = ctx.options.end_date;
    hs->header.crs        = ctx.spatial.crs;

    // Nodes
    const int n_nodes = ctx.n_nodes();
    hs->nodes.resize(static_cast<std::size_t>(n_nodes));
    for (int i = 0; i < n_nodes; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        hs->nodes[ui].id     = ctx.node_names.name_of(i);
        hs->nodes[ui].depth  = ctx.nodes.depth[ui];
        hs->nodes[ui].head   = ctx.nodes.head[ui];
        hs->nodes[ui].volume = ctx.nodes.volume[ui];
        if (ctx.options.water_age &&
            ui < ctx.water_age_state.node_age.size())
            hs->nodes[ui].age = ctx.water_age_state.node_age[ui];
    }

    // Links
    const int n_links = ctx.n_links();
    hs->links.resize(static_cast<std::size_t>(n_links));
    for (int i = 0; i < n_links; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        hs->links[ui].id     = ctx.link_names.name_of(i);
        hs->links[ui].flow   = ctx.links.flow[ui];
        hs->links[ui].depth  = ctx.links.depth[ui];
        hs->links[ui].volume = ctx.links.volume[ui];
        if (ctx.options.water_age &&
            ui < ctx.water_age_state.link_age.size())
            hs->links[ui].age = ctx.water_age_state.link_age[ui];
    }

    // Subcatchments — V2: include infil + GW state
    const int n_sub = ctx.n_subcatches();
    hs->subcatches.resize(static_cast<std::size_t>(n_sub));
    for (int i = 0; i < n_sub; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        auto& rec    = hs->subcatches[ui];
        rec.id       = ctx.subcatch_names.name_of(i);
        rec.runoff   = ctx.subcatches.runoff[ui];
        rec.gwater   = ctx.subcatches.gw_flow[ui];

        // Infiltration state
        if (runoff_solver) {
            runoff_solver->infil_get_state(i, rec.infil_model, rec.infil);
        } else {
            rec.infil_model = 0;
        }

        // GW zone state
        if (gw_solver) {
            const auto& gwa = gw_solver->state();
            if (ui < gwa.theta.size()) {
                rec.gw_theta       = gwa.theta[ui];
                rec.gw_lower_depth = ui < gwa.lower_depth.size()
                                     ? gwa.lower_depth[ui] : 0.0;
            }
        }
    }

    if (!write_file(*hs, path)) {
        delete hs;
        return nullptr;
    }

    return hs;
}

// ============================================================================
// HotStartManager::open()
// ============================================================================

HotStartFile* HotStartManager::open(const std::string& path) {
    tl_last_io_error.clear();

    auto* hs = new HotStartFile();
    if (!read_file(*hs, path)) {
        delete hs;
        return nullptr;
    }
    return hs;
}

// ============================================================================
// HotStartManager::apply()
// ============================================================================

int HotStartManager::apply(HotStartFile& hs,
                           SimulationContext& ctx,
                           std::function<void(const std::string&)> warn_cb) {
    hs.warnings.clear();
    int missing = 0;

    auto emit_warning = [&](const std::string& msg) {
        hs.warnings.push_back(msg);
        if (warn_cb) warn_cb(msg);
        ++missing;
    };

    // Apply node records
    for (const auto& rec : hs.nodes) {
        const int idx = ctx.node_names.find(rec.id);
        if (idx < 0) {
            emit_warning("Hot start: node '" + rec.id + "' not found in current model");
            continue;
        }
        const auto i = static_cast<std::size_t>(idx);
        ctx.nodes.depth[i]  = rec.depth;
        ctx.nodes.head[i]   = rec.head;
        ctx.nodes.volume[i] = rec.volume;
        // A2a: restore water age. Sizing happens once (guarded resize);
        // both engines then seed FROM the loaded state instead of
        // INITIAL_STATE (hotstart_loaded; legacy_seeded suppresses the
        // mirror's first-step fill).
        if (rec.age >= 0.0 && ctx.options.water_age) {
            auto& ws = ctx.water_age_state;
            if (ws.node_age.size() !=
                static_cast<std::size_t>(ctx.n_nodes()))
                ws.resize(ctx.n_nodes(), ctx.n_links());
            ws.node_age[i]     = rec.age;
            ws.hotstart_loaded = true;
            ws.legacy_seeded   = true;
        }
    }

    // Apply link records
    for (const auto& rec : hs.links) {
        const int idx = ctx.link_names.find(rec.id);
        if (idx < 0) {
            emit_warning("Hot start: link '" + rec.id + "' not found in current model");
            continue;
        }
        const auto i = static_cast<std::size_t>(idx);
        ctx.links.flow[i]  = rec.flow;
        ctx.links.depth[i] = rec.depth;
        ctx.links.volume[i] = rec.volume;
        if (rec.age >= 0.0 && ctx.options.water_age) {
            auto& ws = ctx.water_age_state;
            if (ws.link_age.size() !=
                static_cast<std::size_t>(ctx.n_links()))
                ws.resize(ctx.n_nodes(), ctx.n_links());
            ws.link_age[i]     = rec.age;
            ws.hotstart_loaded = true;
            ws.legacy_seeded   = true;
        }
    }

    // Apply subcatchment records — and any V2 solver-internal state via
    // ctx.state_accessors when the file is V2 and accessors are wired.
    const bool apply_v2 = (hs.header.version >= 2u) && ctx.state_accessors.can_write();

    for (const auto& rec : hs.subcatches) {
        const int idx = ctx.subcatch_names.find(rec.id);
        if (idx < 0) {
            emit_warning("Hot start: subcatchment '" + rec.id +
                         "' not found in current model");
            continue;
        }
        const auto i = static_cast<std::size_t>(idx);
        ctx.subcatches.runoff[i] = rec.runoff;
        ctx.subcatches.gw_flow[i] = rec.gwater;

        if (apply_v2) {
            if (ctx.state_accessors.set_infil_state && rec.infil_model >= 0) {
                ctx.state_accessors.set_infil_state(idx, rec.infil_model, rec.infil);
            }
            if (ctx.state_accessors.set_gw_state && rec.gw_theta >= 0.0) {
                ctx.state_accessors.set_gw_state(idx, rec.gw_theta, rec.gw_lower_depth);
            }
        }
    }

    return missing;
}

// ============================================================================
// HotStartManager::apply() — V2 overload with infil + GW state (Gap #54)
// ============================================================================

int HotStartManager::apply(HotStartFile& hs,
                           SimulationContext& ctx,
                           runoff::RunoffSolver* runoff_solver,
                           groundwater::GWSolver* gw_solver,
                           std::function<void(const std::string&)> warn_cb) {
    // Apply hydraulic state (nodes, links) via the existing V1 overload
    int missing = apply(hs, ctx, warn_cb);

    // Apply V2 infiltration + GW state if the file has it
    if (hs.header.version < 2u) return missing;

    for (const auto& rec : hs.subcatches) {
        const int idx = ctx.subcatch_names.find(rec.id);
        if (idx < 0) continue;  // already counted as missing in V1 path

        // Infiltration model state
        if (runoff_solver && rec.infil_model >= 0) {
            runoff_solver->infil_set_state(idx, rec.infil_model, rec.infil);
        }

        // GW zone state
        if (gw_solver && rec.gw_theta >= 0.0) {
            auto& gwa = gw_solver->state();
            const auto ui = static_cast<std::size_t>(idx);
            if (ui < gwa.theta.size())       gwa.theta[ui]       = rec.gw_theta;
            if (ui < gwa.lower_depth.size()) gwa.lower_depth[ui] = rec.gw_lower_depth;
        }
    }

    return missing;
}

// ============================================================================
// HotStartManager::apply_legacy_routing()
// ============================================================================
//
// Reads a legacy EPA SWMM5 `.hsf` file (the format written by the reference
// engine for SAVE/USE HOTSTART) and applies its node/link routing state BY
// OBJECT INDEX. Mirrors legacy hotstart.c initializeFromHotstartFile() +
// readRouting(); see also hotstart_is_valid() for the stamp/version logic.

int HotStartManager::apply_legacy_routing(
        const std::string& path,
        SimulationContext& ctx,
        std::function<void(const std::string&)> warn_cb) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        tl_last_io_error = "Cannot open hotstart file '" + path + "'";
        return 1;
    }

    // --- detect version from the file stamp (hotstart_is_valid) ---
    // Versions 2-4 use a 15-char stamp "SWMM5-HOTSTART<n>"; version 1 uses the
    // 14-char "SWMM5-HOTSTART".
    char stamp[16] = {};
    file.read(stamp, 15);
    if (!file) {
        tl_last_io_error = "Hotstart file '" + path + "' too small for header";
        return 1;
    }
    int version = 0;
    if      (std::memcmp(stamp, "SWMM5-HOTSTART4", 15) == 0) version = 4;
    else if (std::memcmp(stamp, "SWMM5-HOTSTART3", 15) == 0) version = 3;
    else if (std::memcmp(stamp, "SWMM5-HOTSTART2", 15) == 0) version = 2;
    else {
        file.clear();
        file.seekg(0, std::ios::beg);
        char s14[15] = {};
        file.read(s14, 14);
        if (file && std::memcmp(s14, "SWMM5-HOTSTART", 14) == 0) version = 1;
        else {
            tl_last_io_error =
                "'" + path + "' is not a legacy EPA SWMM5 hotstart file";
            return 2;
        }
    }

    // --- header object counts (legacy initializeFromHotstartFile) ---
    int32_t nSub = 0, nLand = 0, nNodes = 0, nLinks = 0, nPollut = 0, flowUnits = 0;
    if (version >= 2) { if (!read_pod(file, nSub))  return 1; } else nSub = ctx.n_subcatches();
    if (version >= 3) { if (!read_pod(file, nLand)) return 1; }
    if (!read_pod(file, nNodes))    return 1;
    if (!read_pod(file, nLinks))    return 1;
    if (!read_pod(file, nPollut))   return 1;
    if (!read_pod(file, flowUnits)) return 1;

    if (nNodes != ctx.n_nodes() || nLinks != ctx.n_links()) {
        tl_last_io_error = "Hotstart node/link count mismatch (file " +
            std::to_string(nNodes) + "/" + std::to_string(nLinks) + " vs model " +
            std::to_string(ctx.n_nodes()) + "/" + std::to_string(ctx.n_links()) + ")";
        return 3;
    }

    // --- runoff (subcatchment) section ---
    // Legacy initializeFromHotstartFile() calls readRunoff() BEFORE readRouting()
    // for fileVersion >= 3, and readRouting() itself consumes an inline 2-float
    // groundwater block per subcatchment for fileVersion == 2. The refactored
    // engine applies only the routing (node/link) state, but it MUST advance the
    // stream past the runoff section to reach the routing records — otherwise
    // every model that has subcatchments (i.e. almost all real models) was
    // rejected outright with "Hot start file error" (#93), even though legacy
    // reads the same file fine.
    //
    // The runoff record layout is NOT self-describing: each subcatchment's
    // length depends on whether it has groundwater and/or a snowpack, exactly as
    // legacy saveRunoff() wrote it. We reconstruct that length from the CURRENT
    // model's structure (gw_aquifer[i] >= 0 mirrors legacy Subcatch[i].
    // groundwater != NULL; snowpack[i] >= 0 mirrors Subcatch[i].snowpack !=
    // NULL) — the same "model must match the file" assumption legacy
    // readRunoff() itself relies on, since the file records subcatchment state
    // positionally with no per-object type tags.
    if (nSub != ctx.n_subcatches()) {
        tl_last_io_error = "Hotstart subcatchment count mismatch (file " +
            std::to_string(nSub) + " vs model " +
            std::to_string(ctx.n_subcatches()) + ")";
        return 3;
    }

    if (nSub > 0) {
        // Routing state IS applied below; subcatchment runoff/infiltration/
        // groundwater/snowpack/buildup state in the file is skipped, not
        // applied — a continuation run is hydraulically hot-started but its
        // hydrology restarts cold. Surface that so it is not silently lossy.
        const std::string msg =
            "USE HOTSTART: subcatchment runoff/infiltration/groundwater/"
            "snowpack state was skipped (routing state applied only)";
        if (warn_cb) warn_cb(msg);
    }

    if (version >= 3 && nSub > 0) {
        // Skip the readRunoff() section. All fields legacy wrote here are
        // 8-byte doubles (saveRunoff). Per subcatchment: 3 sub-area ponded
        // depths + total runoff (4), infiltration state (6), +4 if it has
        // groundwater, +15 (3 surfaces x 5) if it has a snowpack, and — only
        // when pollutants exist — runoff + ponded quality (2*nPollut) plus
        // per-land-use buildup and last-swept (nLand*(nPollut+1)).
        const auto& sub = ctx.subcatches;
        std::streamoff skip_doubles = 0;
        for (int i = 0; i < nSub; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            std::streamoff nd = 4 + 6;
            if (ui < sub.gw_aquifer.size() && sub.gw_aquifer[ui] >= 0) nd += 4;
            if (ui < sub.snowpack.size()   && sub.snowpack[ui]   >= 0) nd += 15;
            if (nPollut > 0) {
                nd += 2 * static_cast<std::streamoff>(nPollut);
                nd += static_cast<std::streamoff>(nLand) *
                      (static_cast<std::streamoff>(nPollut) + 1);
            }
            skip_doubles += nd;
        }
        file.seekg(skip_doubles * static_cast<std::streamoff>(sizeof(double)),
                   std::ios::cur);
        if (!file) {
            tl_last_io_error =
                "Hotstart runoff section truncated in '" + path + "'";
            return 1;
        }
    }
    else if (version == 2 && nSub > 0) {
        // fileVersion 2 has no separate runoff section; legacy readRouting()
        // instead reads 2 floats (GW moisture content, water-table elevation)
        // per subcatchment immediately before the node records.
        file.seekg(static_cast<std::streamoff>(nSub) * 2 *
                   static_cast<std::streamoff>(sizeof(float)),
                   std::ios::cur);
        if (!file) {
            tl_last_io_error =
                "Hotstart v2 groundwater block truncated in '" + path + "'";
            return 1;
        }
    }

    auto& nodes = ctx.nodes;
    auto& links = ctx.links;

    // --- node states (legacy readRouting): depth, lateral inflow, [storage hrt],
    //     pollutant quality. All float, internal units. ---
    for (int i = 0; i < nNodes; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        float depth = 0.0f, lat = 0.0f;
        if (!read_pod(file, depth)) return 1;
        if (!read_pod(file, lat))   return 1;
        nodes.depth[ui]    = static_cast<double>(depth);
        nodes.lat_flow[ui] = static_cast<double>(lat);
        if (version >= 4 && nodes.type[ui] == NodeType::STORAGE) {
            float hrt = 0.0f;
            if (!read_pod(file, hrt)) return 1;  // storage residence time (unused here)
        }
        for (int j = 0; j < nPollut; ++j) { float q = 0.0f; if (!read_pod(file, q)) return 1; }
        if (version <= 2)
            for (int j = 0; j < nPollut; ++j) { float q = 0.0f; if (!read_pod(file, q)) return 1; }
    }

    // --- link states (legacy readRouting): flow, depth, setting, quality. ---
    for (int i = 0; i < nLinks; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        float flow = 0.0f, depth = 0.0f, setting = 0.0f;
        if (!read_pod(file, flow))    return 1;
        if (!read_pod(file, depth))   return 1;
        if (!read_pod(file, setting)) return 1;
        links.flow[ui]           = static_cast<double>(flow);
        links.depth[ui]          = static_cast<double>(depth);
        links.setting[ui]        = static_cast<double>(setting);
        links.target_setting[ui] = static_cast<double>(setting);
        for (int j = 0; j < nPollut; ++j) { float q = 0.0f; if (!read_pod(file, q)) return 1; }
    }

    (void)flowUnits;
    (void)nLand;
    (void)warn_cb;
    tl_last_io_error.clear();
    return 0;
}

// ============================================================================
// HotStartManager::save_legacy_routing()
//
// Op-for-op transliteration of legacy hotstart.c saveHotstart()+saveRouting()
// for the "SWMM5-HOTSTART4" format (hotstart.c:327-353, 358-399).
// ============================================================================

int HotStartManager::save_legacy_routing(const std::string& path,
                                         const SimulationContext& ctx) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        tl_last_io_error = "Cannot open hotstart save file '" + path + "'";
        return 1;
    }

    // --- header: 15-char stamp (no NUL) + object counts (int32) ---
    //     legacy hotstart.c:346-352.
    const char stamp[] = "SWMM5-HOTSTART4";       // 15 chars + implicit NUL
    file.write(stamp, 15);                          // NUL not written (strlen)

    const int32_t nSub   = ctx.n_subcatches();
    const int32_t nLand  = 0;                       // land uses (unused here)
    const int32_t nNodes = ctx.n_nodes();
    const int32_t nLinks = ctx.n_links();
    const int32_t nPollut = ctx.n_pollutants();
    const int32_t flowUnits = static_cast<int32_t>(ctx.options.flow_units);
    if (!write_pod(file, nSub)   || !write_pod(file, nLand) ||
        !write_pod(file, nNodes) || !write_pod(file, nLinks) ||
        !write_pod(file, nPollut) || !write_pod(file, flowUnits)) {
        tl_last_io_error = "Hotstart header write failed for '" + path + "'";
        return 1;
    }

    const auto& nodes = ctx.nodes;
    const auto& links = ctx.links;

    // --- node routing state (legacy saveRouting hotstart.c:368-386):
    //     depth, latFlow [, storage hrt], per-pollutant quality — all float. ---
    for (int i = 0; i < nNodes; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        write_pod(file, static_cast<float>(nodes.depth[ui]));
        write_pod(file, static_cast<float>(nodes.lat_flow[ui]));
        // Version-4 storage residence time. The reader discards it and it does
        // not affect hydraulic routing (quality-only), so 0 is written.
        if (nodes.type[ui] == NodeType::STORAGE)
            write_pod(file, 0.0f);
        for (int j = 0; j < nPollut; ++j) {
            const auto qi = ui * static_cast<std::size_t>(nPollut) +
                            static_cast<std::size_t>(j);
            float q = (qi < nodes.conc.size())
                ? static_cast<float>(nodes.conc[qi]) : 0.0f;
            write_pod(file, q);
        }
    }

    // --- link routing state (legacy saveRouting hotstart.c:387-397):
    //     flow, depth, setting, per-pollutant quality — all float. ---
    for (int i = 0; i < nLinks; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        write_pod(file, static_cast<float>(links.flow[ui]));
        write_pod(file, static_cast<float>(links.depth[ui]));
        write_pod(file, static_cast<float>(links.setting[ui]));
        for (int j = 0; j < nPollut; ++j) {
            const auto qi = ui * static_cast<std::size_t>(nPollut) +
                            static_cast<std::size_t>(j);
            float q = (qi < links.conc.size())
                ? static_cast<float>(links.conc[qi]) : 0.0f;
            write_pod(file, q);
        }
    }

    if (!file) {
        tl_last_io_error = "Hotstart body write failed for '" + path + "'";
        return 1;
    }
    tl_last_io_error.clear();
    return 0;
}

// ============================================================================
// HotStartManager::flush()
// ============================================================================

bool HotStartManager::flush(HotStartFile& hs) {
    if (!hs.dirty) return true;
    if (hs.path.empty()) {
        tl_last_io_error = "HotStartFile has no path; cannot flush";
        return false;
    }
    if (!write_file(hs, hs.path)) return false;
    hs.dirty = false;
    return true;
}

} /* namespace openswmm */
