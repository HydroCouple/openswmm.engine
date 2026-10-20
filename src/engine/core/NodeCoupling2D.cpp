/**
 * @file NodeCoupling2D.cpp
 * @brief [2D_NODE_COUPLING] tokens, class defaults, validation, section
 *        handler and row formatting.
 *
 * @see NodeCoupling2D.hpp
 * @ingroup engine_core
 */

#include "NodeCoupling2D.hpp"

#include "SimulationContext.hpp"
#include "../input/Tokenizer.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace openswmm::nodecoupling2d {

namespace {

constexpr const char* kTypeTokens[] = {"NONE", "MANHOLE", "GRATE", "CURB", "OPEN", "OUTFALL"};

constexpr const char* kKeyTokens[kNodeCoupling2DKeys] = {
    "D", "PERIMETER", "AREA", "CHAMBER_AREA", "LID_OPEN_FRACTION", "CREST",
    "COEFFS", "CW", "CSW", "COI", "COO", "LAW", "SPREAD", "TAILWATER_RELAX"};

constexpr const char* kCrestTokens[]  = {"NODE_RIM", "MESH_BED", "MAX", "MIN"};
constexpr const char* kCoeffsTokens[] = {"DEFAULT", "RUBINATO2017", "RUBINATO2017_LOWER",
                                         "RUBINATO2017_UPPER"};
constexpr const char* kLawTokens[]    = {"ORIFICE", "WEIR_ORIFICE"};

template <std::size_t N>
int findToken(const char* const (&tokens)[N], std::string_view tok) {
    const std::string up = input::Tokenizer::to_upper(tok);
    for (std::size_t i = 0; i < N; ++i)
        if (up == tokens[i]) return static_cast<int>(i);
    return -1;
}

template <std::size_t N>
std::string choices(const char* const (&tokens)[N]) {
    std::string s;
    for (std::size_t i = 0; i < N; ++i) s += (i ? "|" : "") + std::string(tokens[i]);
    return s;
}

bool number(const std::string& tok, double& out) {
    char* end = nullptr;
    out = std::strtod(tok.c_str(), &end);
    return end != tok.c_str() && *end == '\0' && std::isfinite(out);
}

// Shortest %g form that reads back to v.
std::string shortest(double v) {
    char buf[32];
    for (int prec = 6; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    return buf;
}

bool isOutfallKey(NodeCoupling2DKey k) noexcept {
    return k == NodeCoupling2DKey::SPREAD || k == NodeCoupling2DKey::TAILWATER_RELAX;
}

} // namespace

const char* typeToken(NodeCoupling2DType t) noexcept {
    const auto i = static_cast<int>(t);
    return (i >= 0 && i < 6) ? kTypeTokens[i] : "NONE";
}

bool parseType(std::string_view token, NodeCoupling2DType& out) {
    const int i = findToken(kTypeTokens, token);
    if (i < 0) return false;
    out = static_cast<NodeCoupling2DType>(i);
    return true;
}

const char* keyToken(NodeCoupling2DKey k) noexcept {
    const auto i = static_cast<int>(k);
    return (i >= 0 && i < kNodeCoupling2DKeys) ? kKeyTokens[i] : "";
}

bool parseKey(std::string_view token, NodeCoupling2DKey& out) {
    const int i = findToken(kKeyTokens, token);
    if (i < 0) return false;
    out = static_cast<NodeCoupling2DKey>(i);
    return true;
}

std::string parseValue(NodeCoupling2DKey k, const std::string& token, double& out) {
    const std::string key = keyToken(k);
    int i = -1;
    switch (k) {
        case NodeCoupling2DKey::CREST:
            if ((i = findToken(kCrestTokens, token)) < 0)
                return key + " must be " + choices(kCrestTokens) + ": " + token;
            out = i;
            return {};
        case NodeCoupling2DKey::COEFFS:
            if ((i = findToken(kCoeffsTokens, token)) < 0)
                return key + " must be " + choices(kCoeffsTokens) + ": " + token;
            out = i;
            return {};
        case NodeCoupling2DKey::LAW:
            if ((i = findToken(kLawTokens, token)) < 0)
                return key + " must be " + choices(kLawTokens) + ": " + token;
            out = i;
            return {};
        default:
            break;
    }
    if (!number(token, out)) return key + " must be a number: " + token;
    switch (k) {
        case NodeCoupling2DKey::LID_OPEN_FRACTION:
            if (!(out > 0.0 && out <= 1.0)) return key + " must be in (0, 1]: " + token;
            break;
        case NodeCoupling2DKey::SPREAD:
        case NodeCoupling2DKey::TAILWATER_RELAX:
            if (out < 0.0) return key + " must be >= 0: " + token;
            break;
        default:
            if (!(out > 0.0)) return key + " must be > 0: " + token;
            break;
    }
    return {};
}

std::string formatValue(NodeCoupling2DKey k, double v) {
    const int i = static_cast<int>(v);
    switch (k) {
        case NodeCoupling2DKey::CREST:  return (i >= 0 && i < 4) ? kCrestTokens[i] : "";
        case NodeCoupling2DKey::COEFFS: return (i >= 0 && i < 4) ? kCoeffsTokens[i] : "";
        case NodeCoupling2DKey::LAW:    return (i >= 0 && i < 2) ? kLawTokens[i] : "";
        default:                        return shortest(v);
    }
}

bool keyAllowed(NodeCoupling2DType t, NodeCoupling2DKey k) noexcept {
    switch (t) {
        case NodeCoupling2DType::NONE:    return false;
        case NodeCoupling2DType::OUTFALL: return isOutfallKey(k);
        default:                          return !isOutfallKey(k);
    }
}

NodeCoupling2DType defaultType(const SimulationContext& ctx, int node) {
    const auto un = static_cast<std::size_t>(node);
    switch (ctx.nodes.type[un]) {
        case NodeType::OUTFALL: return NodeCoupling2DType::OUTFALL;
        case NodeType::STORAGE: return NodeCoupling2DType::OPEN;
        case NodeType::DIVIDER: return NodeCoupling2DType::MANHOLE;
        case NodeType::JUNCTION: break;
    }
    // Inlet junctions already route street flow into the sewer through
    // [INLETS]; coupling them to the mesh too would route it twice (D6).
    // Virtual junctions have no rim.
    const bool inlet   = un < ctx.nodes.is_inlet.size()   && ctx.nodes.is_inlet[un] != 0;
    const bool virtual_ = un < ctx.nodes.is_virtual.size() && ctx.nodes.is_virtual[un] != 0;
    return (inlet || virtual_) ? NodeCoupling2DType::NONE : NodeCoupling2DType::MANHOLE;
}

NodeCoupling2DType effectiveType(const SimulationContext& ctx, int node) {
    const int r = ctx.node_coupling_2d.row_of(node);
    return r >= 0 ? ctx.node_coupling_2d.type[static_cast<std::size_t>(r)]
                  : defaultType(ctx, node);
}

std::string checkType(const SimulationContext& ctx, int node, NodeCoupling2DType t) {
    const bool outfall = ctx.nodes.type[static_cast<std::size_t>(node)] == NodeType::OUTFALL;
    if (outfall && t != NodeCoupling2DType::OUTFALL && t != NodeCoupling2DType::NONE)
        return std::string("an outfall couples as OUTFALL or NONE, not ") + typeToken(t);
    if (!outfall && t == NodeCoupling2DType::OUTFALL)
        return "type OUTFALL needs an outfall node";
    if (t == NodeCoupling2DType::OPEN &&
        ctx.nodes.type[static_cast<std::size_t>(node)] != NodeType::STORAGE)
        return "type OPEN needs a storage node";
    return {};
}

void readSection(SimulationContext& ctx, const std::vector<std::string>& lines) {
    auto& rows = ctx.node_coupling_2d;
    for (const auto& line : lines) {
        const auto t = input::Tokenizer::tokenize(line);
        if (t.empty()) continue;
        auto fail = [&](const std::string& why) {
            ctx.errors.push_back("[2D_NODE_COUPLING] " + why + " — line: " + line);
        };
        if (t.size() < 2) { fail("expected NODE TYPE [KEY=VALUE ...]"); continue; }
        const int n = ctx.node_names.find(t[0]);
        if (n < 0) { ctx.deferred_section_rows.emplace_back("2D_NODE_COUPLING", line); continue; }
        NodeCoupling2DType type{};
        if (!parseType(t[1], type)) {
            fail("unknown type " + t[1] + " (expected " + choices(kTypeTokens) + ")");
            continue;
        }
        if (auto why = checkType(ctx, n, type); !why.empty()) { fail(t[0] + ": " + why); continue; }
        if (rows.row_of(n) >= 0) { fail("duplicate row for node " + t[0]); continue; }

        NodeCoupling2DParams params = unsetNodeCoupling2DParams();
        std::string err;
        for (std::size_t i = 2; i < t.size() && err.empty(); ++i) {
            const auto eq = t[i].find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 == t[i].size()) {
                err = "expected KEY=VALUE: " + t[i];
                break;
            }
            NodeCoupling2DKey key{};
            if (!parseKey(t[i].substr(0, eq), key)) { err = "unknown key " + t[i].substr(0, eq); break; }
            if (!keyAllowed(type, key)) {
                err = std::string(keyToken(key)) + " does not apply to type " + typeToken(type);
                break;
            }
            auto& slot = params[static_cast<std::size_t>(key)];
            if (!std::isnan(slot)) { err = std::string("duplicate key ") + keyToken(key); break; }
            err = parseValue(key, t[i].substr(eq + 1), slot);
        }
        if (!err.empty()) { fail(err); continue; }

        rows.node.push_back(n);
        rows.type.push_back(type);
        rows.params.push_back(params);
    }
}

std::string formatParams(const SimulationContext& ctx, int r) {
    const auto& p = ctx.node_coupling_2d.params[static_cast<std::size_t>(r)];
    std::string text;
    for (int k = 0; k < kNodeCoupling2DKeys; ++k) {
        const double v = p[static_cast<std::size_t>(k)];
        if (std::isnan(v)) continue;
        const auto key = static_cast<NodeCoupling2DKey>(k);
        text += (text.empty() ? "" : " ") + std::string(keyToken(key)) + "=" + formatValue(key, v);
    }
    return text;
}

std::string formatRow(const SimulationContext& ctx, int r) {
    const auto ur = static_cast<std::size_t>(r);
    const auto& rows = ctx.node_coupling_2d;
    char head[96];
    std::snprintf(head, sizeof head, "%-16s %-8s",
                  ctx.node_names.name_of(rows.node[ur]).c_str(), typeToken(rows.type[ur]));
    std::string line = head;
    const std::string params = formatParams(ctx, r);
    if (!params.empty()) line += " " + params;
    while (!line.empty() && line.back() == ' ') line.pop_back();
    return line;
}

} // namespace openswmm::nodecoupling2d
