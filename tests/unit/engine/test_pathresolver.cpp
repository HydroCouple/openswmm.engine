/**
 * @file test_pathresolver.cpp
 * @brief Unit tests for openswmm::io::PathResolver.
 *
 * @details Covers the four documented entry points
 *   (makeRelative / resolveRelative / normaliseSeparators / parentDir)
 *   across the corner cases listed in IO_PORTABILITY_PLAN §1A:
 *
 *     - relative ⇄ absolute round-trip on POSIX-style paths
 *     - Windows drive-letter cross-volume detection
 *     - UNC root handling
 *     - ".." depth cap
 *     - separator normalisation (backslash → forward-slash, `./` collapse,
 *       trailing-slash trimming, drive-root preservation)
 *     - missing-file safety (no canonical() calls; no exceptions)
 *     - Unicode round-trip
 *
 *   Per CLAUDE.md §4.1 ("Transparent File IO") any on-disk fixture is
 *   written under tests/unit/engine/data/pathresolver/ — never the OS
 *   temp directory — so a reviewer can inspect the artifacts.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>

#include "../../src/engine/core/PathResolver.hpp"

using openswmm::io::PathClass;
using openswmm::io::RelativeResult;
using openswmm::io::makeRelative;
using openswmm::io::resolveRelative;
using openswmm::io::normaliseSeparators;
using openswmm::io::parentDir;
using openswmm::io::isAbsolutePath;

// ---------------------------------------------------------------------------
// normaliseSeparators
// ---------------------------------------------------------------------------

TEST(PathResolverNormalise, ConvertsBackslashesToForwardSlashes) {
    EXPECT_EQ(normaliseSeparators("a\\b\\c"), "a/b/c");
    EXPECT_EQ(normaliseSeparators("C:\\proj\\model.inp"), "C:/proj/model.inp");
}

TEST(PathResolverNormalise, CollapsesDuplicateSlashes) {
    EXPECT_EQ(normaliseSeparators("a//b///c"), "a/b/c");
    EXPECT_EQ(normaliseSeparators("/a///b"),   "/a/b");
}

TEST(PathResolverNormalise, DropsDotSegments) {
    EXPECT_EQ(normaliseSeparators("a/./b/./c"), "a/b/c");
    EXPECT_EQ(normaliseSeparators("./a/b"),     "a/b");
    EXPECT_EQ(normaliseSeparators("a/b/."),     "a/b");
}

TEST(PathResolverNormalise, TrimsTrailingSlashExceptRoots) {
    EXPECT_EQ(normaliseSeparators("a/b/"),   "a/b");
    EXPECT_EQ(normaliseSeparators("/"),      "/");
    EXPECT_EQ(normaliseSeparators("C:/"),    "C:/");
}

TEST(PathResolverNormalise, EmptyInputReturnsEmpty) {
    EXPECT_EQ(normaliseSeparators(""), "");
}

TEST(PathResolverNormalise, IsIdempotent) {
    const std::string raw  = "a\\b//c/./d/";
    const std::string once = normaliseSeparators(raw);
    EXPECT_EQ(normaliseSeparators(once), once);
}

TEST(PathResolverNormalise, PreservesUnicode) {
    // Cyrillic, Japanese, Greek — sanity-check that we don't mangle bytes
    // outside the ASCII separator alphabet.
    const std::string cyrillic = "/Пользователи/проект/модель.inp";
    EXPECT_EQ(normaliseSeparators(cyrillic), cyrillic);
    const std::string japanese = "/ホーム/水文/モデル.inp";
    EXPECT_EQ(normaliseSeparators(japanese), japanese);
}

// ---------------------------------------------------------------------------
// isAbsolutePath
// ---------------------------------------------------------------------------

TEST(PathResolverIsAbsolute, RecognisesPosixAbsolute) {
    EXPECT_TRUE(isAbsolutePath("/a/b"));
    EXPECT_FALSE(isAbsolutePath("a/b"));
    EXPECT_FALSE(isAbsolutePath(""));
}

TEST(PathResolverIsAbsolute, RecognisesWindowsDriveLetter) {
    EXPECT_TRUE(isAbsolutePath("C:\\foo"));
    EXPECT_TRUE(isAbsolutePath("C:/foo"));
    EXPECT_TRUE(isAbsolutePath("Z:foo"));   // "Z:foo" is drive-relative but
                                            // we still consider it root-bound.
}

TEST(PathResolverIsAbsolute, RecognisesUNC) {
    EXPECT_TRUE(isAbsolutePath("\\\\server\\share\\foo"));
    EXPECT_TRUE(isAbsolutePath("//server/share/foo"));
}

// ---------------------------------------------------------------------------
// parentDir
// ---------------------------------------------------------------------------

TEST(PathResolverParentDir, ReturnsParentOfFile) {
    EXPECT_EQ(parentDir("/a/b/c.inp"), "/a/b");
    EXPECT_EQ(parentDir("/a/b/"),      "/a/b");
}

TEST(PathResolverParentDir, EmptyWhenNoParent) {
    EXPECT_EQ(parentDir("model.inp"), "");
    EXPECT_EQ(parentDir(""),          "");
}

TEST(PathResolverParentDir, PreservesPosixRoot) {
    EXPECT_EQ(parentDir("/foo"), "/");
}

TEST(PathResolverParentDir, PreservesWindowsDriveRoot) {
    EXPECT_EQ(parentDir("C:/foo"), "C:/");
}

TEST(PathResolverParentDir, HandlesBackslashSeparators) {
    EXPECT_EQ(parentDir("C:\\proj\\model.inp"), "C:/proj");
}

// ---------------------------------------------------------------------------
// resolveRelative
// ---------------------------------------------------------------------------

TEST(PathResolverResolve, EmptyTokenReturnsEmpty) {
    EXPECT_EQ(resolveRelative("", "/a/b"), "");
}

TEST(PathResolverResolve, AbsoluteTokenReturnedUnchanged) {
    // resolveRelative normalises to a portable forward-slash form on EVERY
    // platform (it emits generic_string(), not make_preferred()), so a Windows
    // drive token keeps forward slashes rather than being converted to "\".
#ifdef _WIN32
    EXPECT_EQ(resolveRelative("C:/data/rain.dat", "/a/b"), "C:/data/rain.dat");
#else
    EXPECT_EQ(resolveRelative("/data/rain.dat", "/a/b"), "/data/rain.dat");
#endif
}

TEST(PathResolverResolve, RelativeTokenJoinedToAnchor) {
    // Joined output is also forward-slash on every platform (see above).
#ifdef _WIN32
    EXPECT_EQ(resolveRelative("data/rain.dat", "C:/proj"),
              "C:/proj/data/rain.dat");
#else
    EXPECT_EQ(resolveRelative("data/rain.dat", "/home/me/proj"),
              "/home/me/proj/data/rain.dat");
#endif
}

TEST(PathResolverResolve, AcceptsBackslashSeparatorsInToken) {
#ifndef _WIN32
    // Even on POSIX, an .inp authored on Windows might carry "data\rain.dat".
    EXPECT_EQ(resolveRelative("data\\rain.dat", "/home/me/proj"),
              "/home/me/proj/data/rain.dat");
#endif
}

TEST(PathResolverResolve, CollapsesParentTraversal) {
#ifndef _WIN32
    EXPECT_EQ(resolveRelative("../shared/rain.dat", "/home/me/proj"),
              "/home/me/shared/rain.dat");
    EXPECT_EQ(resolveRelative("../../shared/data/rain.dat",
                              "/home/me/proj/sub"),
              "/home/me/shared/data/rain.dat");
#endif
}

TEST(PathResolverResolve, EmptyAnchorLeavesRelativeUnchanged) {
    // Path is normalised but not absolutised.
    const std::string out = resolveRelative("./data/rain.dat", "");
    EXPECT_NE(out.find("data"), std::string::npos);
    EXPECT_NE(out.find("rain.dat"), std::string::npos);
}

TEST(PathResolverResolve, MissingFileDoesNotThrow) {
    // Resolution must be lexical — no filesystem stat.
    EXPECT_NO_THROW(
        (void) resolveRelative("does/not/exist.dat", "/no/such/place"));
}

// ---------------------------------------------------------------------------
// makeRelative — simple cases
// ---------------------------------------------------------------------------

#ifndef _WIN32
TEST(PathResolverMakeRelative, SiblingFileReturnsBareName) {
    auto r = makeRelative("/proj/data/rain.dat", "/proj/data");
    EXPECT_EQ(r.classification, PathClass::Relative);
    EXPECT_EQ(r.path,           "rain.dat");
    EXPECT_EQ(r.up_levels,      0);
    EXPECT_TRUE(r.warning.empty());
}

TEST(PathResolverMakeRelative, SubdirFile) {
    auto r = makeRelative("/proj/data/rain.dat", "/proj");
    EXPECT_EQ(r.classification, PathClass::Relative);
    EXPECT_EQ(r.path,           "data/rain.dat");
    EXPECT_EQ(r.up_levels,      0);
}

TEST(PathResolverMakeRelative, ParentDirFile) {
    auto r = makeRelative("/shared/rain.dat", "/shared/proj");
    EXPECT_EQ(r.classification, PathClass::Relative);
    EXPECT_EQ(r.path,           "../rain.dat");
    EXPECT_EQ(r.up_levels,      1);
}

TEST(PathResolverMakeRelative, GrandparentTraversal) {
    auto r = makeRelative("/a/shared/data/rain.dat", "/a/proj/sub");
    EXPECT_EQ(r.classification, PathClass::Relative);
    EXPECT_EQ(r.path,           "../../shared/data/rain.dat");
    EXPECT_EQ(r.up_levels,      2);
}
#endif

// ---------------------------------------------------------------------------
// makeRelative — always uses forward-slash on output
// ---------------------------------------------------------------------------

TEST(PathResolverMakeRelative, OutputUsesForwardSlashes) {
#ifdef _WIN32
    auto r = makeRelative("C:\\proj\\data\\rain.dat", "C:\\proj");
    EXPECT_EQ(r.classification, PathClass::Relative);
    // No backslashes in the portable form.
    EXPECT_EQ(r.path.find('\\'), std::string::npos);
    EXPECT_EQ(r.path, "data/rain.dat");
#else
    auto r = makeRelative("/proj/data/rain.dat", "/proj");
    EXPECT_EQ(r.path.find('\\'), std::string::npos);
#endif
}

// ---------------------------------------------------------------------------
// makeRelative — depth cap
// ---------------------------------------------------------------------------

#ifndef _WIN32
TEST(PathResolverMakeRelative, DepthCapTriggersAbsoluteFallback) {
    // 6-level deep anchor; target right under root → 6 up-levels needed.
    auto r = makeRelative("/etc/rain.dat", "/a/b/c/d/e/f", /*max_up*/ 4);
    EXPECT_EQ(r.classification, PathClass::AbsoluteSameVolume);
    EXPECT_FALSE(r.warning.empty());
    EXPECT_NE(r.warning.find("'..' levels"), std::string::npos);
    EXPECT_EQ(r.path, "/etc/rain.dat");
}

TEST(PathResolverMakeRelative, DepthCapDefaultAllowsCommonCases) {
    auto r = makeRelative("/etc/rain.dat", "/a/b/c/d/e/f");
    EXPECT_EQ(r.classification, PathClass::Relative);
    EXPECT_GE(r.up_levels, 6);
    EXPECT_LE(r.up_levels, 16);
}
#endif

// ---------------------------------------------------------------------------
// makeRelative — cross-volume
// ---------------------------------------------------------------------------

TEST(PathResolverMakeRelative, WindowsCrossDriveReturnsAbsoluteCrossVolume) {
    auto r = makeRelative("D:/data/rain.dat", "C:/proj");
    EXPECT_EQ(r.classification, PathClass::AbsoluteCrossVolume);
    EXPECT_FALSE(r.warning.empty());
    EXPECT_NE(r.warning.find("different volume"), std::string::npos);
}

TEST(PathResolverMakeRelative, UNCDifferentShareReturnsAbsoluteCrossVolume) {
    auto r = makeRelative("//srv1/share/data/rain.dat",
                          "//srv2/share/proj");
    EXPECT_EQ(r.classification, PathClass::AbsoluteCrossVolume);
}

TEST(PathResolverMakeRelative, UNCSameShareIsRelative) {
    auto r = makeRelative("//srv/share/data/rain.dat",
                          "//srv/share/proj");
    EXPECT_EQ(r.classification, PathClass::Relative);
    EXPECT_EQ(r.path,           "../data/rain.dat");
}

TEST(PathResolverMakeRelative, DriveLetterCaseInsensitive) {
    // Windows drive letters are case-insensitive — "c:" and "C:" should
    // be treated as the same volume.
    auto r = makeRelative("c:/proj/data/rain.dat", "C:/proj");
    EXPECT_EQ(r.classification, PathClass::Relative);
    EXPECT_EQ(r.path,           "data/rain.dat");
}

// ---------------------------------------------------------------------------
// makeRelative — invalid inputs
// ---------------------------------------------------------------------------

TEST(PathResolverMakeRelative, EmptyTargetIsInvalid) {
    auto r = makeRelative("", "/proj");
    EXPECT_EQ(r.classification, PathClass::Invalid);
    EXPECT_FALSE(r.warning.empty());
}

TEST(PathResolverMakeRelative, EmptyAnchorReturnsAbsoluteWithWarning) {
#ifndef _WIN32
    auto r = makeRelative("/proj/data/rain.dat", "");
    EXPECT_EQ(r.classification, PathClass::AbsoluteSameVolume);
    EXPECT_FALSE(r.warning.empty());
#endif
}

// ---------------------------------------------------------------------------
// makeRelative — Unicode round-trip
// ---------------------------------------------------------------------------

#ifndef _WIN32
TEST(PathResolverMakeRelative, UnicodePathRoundTrip) {
    const std::string target = "/Пользователи/проект/data/дождь.dat";
    const std::string anchor = "/Пользователи/проект";
    auto r = makeRelative(target, anchor);
    EXPECT_EQ(r.classification, PathClass::Relative);
    EXPECT_EQ(r.path,           "data/дождь.dat");
}
#endif

// ---------------------------------------------------------------------------
// makeRelative — exact-match anchor (target == anchor's file)
// ---------------------------------------------------------------------------

#ifndef _WIN32
TEST(PathResolverMakeRelative, ExactMatchYieldsCurrentDir) {
    auto r = makeRelative("/proj/model.inp", "/proj/model.inp");
    EXPECT_EQ(r.classification, PathClass::Relative);
    EXPECT_EQ(r.path,           ".");
    EXPECT_EQ(r.up_levels,      0);
}
#endif

// ---------------------------------------------------------------------------
// resolveRelative ↔ makeRelative round-trip
// ---------------------------------------------------------------------------

#ifndef _WIN32
TEST(PathResolverRoundTrip, MakeThenResolveReturnsOriginal) {
    const std::string target = "/proj/data/rain.dat";
    const std::string anchor = "/proj/sub";

    auto rel = makeRelative(target, anchor);
    ASSERT_EQ(rel.classification, PathClass::Relative);

    const std::string back = resolveRelative(rel.path, anchor);
    EXPECT_EQ(back, target);
}

TEST(PathResolverRoundTrip, ParentTraversalRoundTrips) {
    const std::string target = "/shared/data/rain.dat";
    const std::string anchor = "/shared/proj/sub";

    auto rel = makeRelative(target, anchor);
    ASSERT_EQ(rel.classification, PathClass::Relative);
    EXPECT_EQ(rel.path, "../../data/rain.dat");

    const std::string back = resolveRelative(rel.path, anchor);
    EXPECT_EQ(back, target);
}
#endif
