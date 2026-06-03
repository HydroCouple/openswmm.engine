/**
 * @file PathResolver.cpp
 * @brief PathResolver implementation — see PathResolver.hpp.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "PathResolver.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace openswmm::io {

// ============================================================================
// Internal helpers
// ============================================================================

namespace {

// Convert backslashes to forward-slashes in-place. We do NOT touch any other
// characters; quoted paths, embedded colons (drive letters, CSV column refs)
// flow through untouched.
std::string toForwardSlashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// Drop a trailing slash if and only if removing it leaves a non-empty,
// non-root path. `/`, `C:/`, `//server/share` stay as-is.
std::string trimTrailingSlash(std::string s) {
    if (s.size() <= 1) return s;
    // Preserve drive root (e.g. "C:/")
    if (s.size() == 3 && s[1] == ':' && (s[2] == '/' || s[2] == '\\')) return s;
    // Preserve UNC root "//server/share" — only strip if there's a path beyond.
    if (s.size() >= 2 && (s[0] == '/' && s[1] == '/')) {
        // Count '/' after the leading "//"; need at least 3 total to be
        // beyond the share root before trimming.
        std::size_t slashes = 0;
        for (char c : s) if (c == '/') ++slashes;
        if (slashes < 4) return s;
    }
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

// Collapse "//" into "/" (except in a leading UNC "\\\\?\\" or "//server/").
// Drop redundant "./" segments. Does NOT resolve "..".
std::string collapseSlashesAndDots(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const bool unc = s.size() >= 2 && s[0] == '/' && s[1] == '/';
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '/') {
            // Preserve the leading "//..." UNC marker once.
            if (unc && i == 1 && out.size() == 1 && out.back() == '/') {
                out.push_back(c);
                continue;
            }
            // Otherwise collapse consecutive slashes.
            if (!out.empty() && out.back() == '/') continue;
            out.push_back('/');
        } else {
            out.push_back(c);
        }
    }
    // Drop "./" segments. Repeat until stable.
    for (;;) {
        auto dot_slash = out.find("/./");
        if (dot_slash == std::string::npos) break;
        out.erase(dot_slash, 2);  // remove "/."
    }
    // Trim leading "./" if present.
    if (out.size() >= 2 && out[0] == '.' && out[1] == '/') out.erase(0, 2);
    // Trim trailing "/." if present.
    if (out.size() >= 2 && out[out.size() - 2] == '/' && out.back() == '.')
        out.pop_back(), out.pop_back();
    return out;
}

// Return the root_name of a path string. Recognises Windows drive letters
// ("C:") and UNC roots ("//server/share") regardless of host platform —
// the .inp format may carry such tokens on any host.
std::string rootName(const std::string& path) {
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0]))
                          && path[1] == ':') {
        // "C:" or "C:/..."
        std::string r = path.substr(0, 2);
        r[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(r[0])));
        return r;
    }
    if (path.size() >= 2 && (path[0] == '/' || path[0] == '\\')
                          && (path[1] == '/' || path[1] == '\\')) {
        // UNC: "//server/share/..." — capture through the second separator
        // after the leading "//".
        std::size_t i = 2;
        while (i < path.size() && path[i] != '/' && path[i] != '\\') ++i;
        if (i < path.size()) {
            // consume one separator
            ++i;
            while (i < path.size() && path[i] != '/' && path[i] != '\\') ++i;
        }
        return path.substr(0, i);
    }
    return {};
}

// Count leading "../" segments after normalisation. Stops at the first
// segment that isn't "..".
int countUpLevels(const std::string& normalised) {
    int n = 0;
    std::size_t i = 0;
    while (i + 2 <= normalised.size()
           && normalised[i] == '.' && normalised[i + 1] == '.') {
        // Must be followed by '/' or end-of-string
        if (i + 2 == normalised.size()) { ++n; break; }
        if (normalised[i + 2] != '/') break;
        ++n;
        i += 3;
    }
    return n;
}

} // anonymous namespace

// ============================================================================
// isAbsolutePath
// ============================================================================

bool isAbsolutePath(const std::string& path) noexcept {
    if (path.empty()) return false;
    // POSIX absolute
    if (path[0] == '/') return true;
    // Windows drive ("C:..." or "C:/..."  or "C:\...")
    if (path.size() >= 2
        && std::isalpha(static_cast<unsigned char>(path[0]))
        && path[1] == ':') return true;
    // Windows UNC
    if (path.size() >= 2
        && (path[0] == '\\' || path[0] == '/')
        && (path[1] == '\\' || path[1] == '/')) return true;
    return false;
}

// ============================================================================
// normaliseSeparators
// ============================================================================

std::string normaliseSeparators(const std::string& path) {
    if (path.empty()) return path;
    std::string out = toForwardSlashes(path);
    out = collapseSlashesAndDots(out);
    out = trimTrailingSlash(std::move(out));
    return out;
}

// ============================================================================
// parentDir
// ============================================================================

std::string parentDir(const std::string& file_path) {
    if (file_path.empty()) return {};
    // A trailing separator means the input is already a directory: return it
    // unchanged (normaliseSeparators drops the trailing slash, preserving
    // filesystem roots like "/" or "C:/").
    const char last = file_path.back();
    const std::string norm = normaliseSeparators(file_path);
    if (last == '/' || last == '\\') return norm;
    auto pos = norm.find_last_of('/');
    if (pos == std::string::npos) return {};
    // Preserve drive root ("C:/") and UNC share root.
    if (pos < 2) return norm.substr(0, pos + 1);  // POSIX "/foo" → "/"
    // For "C:/foo" → "C:/"
    if (pos == 2 && norm.size() >= 3 && norm[1] == ':') return norm.substr(0, 3);
    return norm.substr(0, pos);
}

// ============================================================================
// resolveRelative
// ============================================================================

std::string resolveRelative(const std::string& stored_token,
                            const std::string& anchor_dir) {
    if (stored_token.empty()) return {};

    // Already absolute → just normalise to platform-preferred form.
    if (isAbsolutePath(stored_token)) {
        fs::path p(toForwardSlashes(stored_token));
        return p.make_preferred().string();
    }

    // No anchor → return as-is (caller owns the relative-token semantics).
    if (anchor_dir.empty()) {
        fs::path p(toForwardSlashes(stored_token));
        return p.lexically_normal().make_preferred().string();
    }

    // Join anchor + token, lexically normalise (collapses "..").
    fs::path joined = fs::path(toForwardSlashes(anchor_dir))
                    / fs::path(toForwardSlashes(stored_token));
    return joined.lexically_normal().make_preferred().string();
}

// ============================================================================
// makeRelative
// ============================================================================

RelativeResult makeRelative(const std::string& target_absolute,
                            const std::string& anchor_dir,
                            int                max_up_levels) {
    RelativeResult r;

    if (target_absolute.empty()) {
        r.classification = PathClass::Invalid;
        r.warning = "empty target path";
        return r;
    }

    // No anchor → can't compute relative; return target unchanged.
    if (anchor_dir.empty()) {
        r.path = normaliseSeparators(target_absolute);
        r.classification = isAbsolutePath(target_absolute)
                             ? PathClass::AbsoluteSameVolume
                             : PathClass::Invalid;
        if (r.classification != PathClass::Relative)
            r.warning = "no anchor directory provided";
        return r;
    }

    const std::string tgt_fs = toForwardSlashes(target_absolute);
    const std::string anc_fs = toForwardSlashes(anchor_dir);

    // Cross-volume check on root_name. Compared case-insensitively for the
    // drive letter case; UNC shares are compared case-sensitively (server
    // names traditionally are, share names are not, but we err toward
    // diff-strictness — false positives here just degrade to absolute).
    const std::string tgt_root = rootName(tgt_fs);
    const std::string anc_root = rootName(anc_fs);
    auto eq_ci = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i]))
                != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    };
    // Cross-volume whenever the roots differ — including a Windows drive / UNC
    // target against a rootless POSIX anchor (one root empty, the other not).
    // Only when both roots are empty (POSIX vs POSIX) do we treat them as the
    // same volume and fall through to the relative computation below.
    if (!eq_ci(tgt_root, anc_root)) {
        r.path = fs::path(tgt_fs).make_preferred().string();
        r.classification = PathClass::AbsoluteCrossVolume;
        r.warning = "target on a different volume ('" + tgt_root
                  + "' vs '" + anc_root + "') — cannot express relatively";
        return r;
    }

    // Strip the platform-agnostic root before handing the remainder to
    // fs::path::lexically_proximate. This sidesteps cross-platform fs::path
    // quirks (Linux's std::filesystem treats "C:/..." and "//srv/share/..."
    // as relative paths, so proximate against them is unreliable).  After
    // stripping, both strings look like POSIX-absolute paths and proximate
    // behaves uniformly across platforms.
    std::string tgt_body = tgt_fs;
    std::string anc_body = anc_fs;
    if (!tgt_root.empty()) tgt_body.erase(0, tgt_root.size());
    if (!anc_root.empty()) anc_body.erase(0, anc_root.size());
    // Ensure a leading '/' so fs::path treats them as absolute.
    if (tgt_body.empty() || tgt_body.front() != '/') tgt_body.insert(0, "/");
    if (anc_body.empty() || anc_body.front() != '/') anc_body.insert(0, "/");

    fs::path tgt(tgt_body);
    fs::path anc(anc_body);
    fs::path prox = tgt.lexically_proximate(anc);

    if (prox.empty() || prox.is_absolute()) {
        r.path = fs::path(tgt_fs).make_preferred().string();
        r.classification = PathClass::AbsoluteSameVolume;
        r.warning = "lexical relative form unavailable";
        return r;
    }

    std::string out = prox.generic_string();   // forward slashes
    out = collapseSlashesAndDots(out);
    // lexically_proximate may return "" for an exact match; surface as "."
    if (out.empty()) out = ".";

    const int up = countUpLevels(out);
    if (up > max_up_levels) {
        r.path = fs::path(tgt_fs).make_preferred().string();
        r.classification = PathClass::AbsoluteSameVolume;
        r.warning = "relative form exceeds " + std::to_string(max_up_levels)
                  + " '..' levels (would be " + std::to_string(up) + ")";
        return r;
    }

    r.path = std::move(out);
    r.classification = PathClass::Relative;
    r.up_levels = up;
    return r;
}

} // namespace openswmm::io
