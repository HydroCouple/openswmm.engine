/**
 * @file test_name_index.cpp
 * @brief Unit tests for NameIndex case-insensitive name↔index registry.
 *
 * @details Legacy EPA SWMM's hash table (src/legacy/engine/hash.c) matches
 *          object names case-insensitively; NameIndex must do the same while
 *          preserving the original spelling for round-trip output.
 *
 * @see src/engine/data/NameIndex.hpp
 * @ingroup engine_data
 */

#include <gtest/gtest.h>

#include <stdexcept>

#include "../../src/engine/data/NameIndex.hpp"

using openswmm::NameIndex;

TEST(NameIndex, FindIsCaseInsensitive) {
    NameIndex idx;
    EXPECT_EQ(idx.add("ChestnutHillRes_IM"), 0);
    EXPECT_EQ(idx.add("J12"), 1);

    EXPECT_EQ(idx.find("ChestnutHillRes_IM"), 0);
    EXPECT_EQ(idx.find("ChestnuthillRes_IM"), 0);  // the MWRA deck typo
    EXPECT_EQ(idx.find("CHESTNUTHILLRES_IM"), 0);
    EXPECT_EQ(idx.find("j12"), 1);
    EXPECT_EQ(idx.find("nope"), -1);

    EXPECT_TRUE(idx.try_find("chestnuthillres_im").has_value());
    EXPECT_EQ(*idx.try_find("chestnuthillres_im"), 0);
    EXPECT_FALSE(idx.try_find("nope").has_value());
}

TEST(NameIndex, OriginalSpellingIsPreserved) {
    NameIndex idx;
    idx.add("MixedCase_Name");
    EXPECT_EQ(idx.name_of(0), "MixedCase_Name");
    EXPECT_EQ(idx.names()[0], "MixedCase_Name");

    const std::string* canon = idx.canonical("mixedcase_name");
    ASSERT_NE(canon, nullptr);
    EXPECT_EQ(*canon, "MixedCase_Name");
    EXPECT_EQ(idx.canonical("absent"), nullptr);
}

TEST(NameIndex, CaseVariantAddIsDuplicate) {
    NameIndex idx;
    EXPECT_EQ(idx.try_add("J1"), 0);
    EXPECT_EQ(idx.try_add("J1"), -1);   // exact duplicate
    EXPECT_EQ(idx.try_add("j1"), -1);   // case-variant duplicate
    EXPECT_EQ(idx.size(), 1);
    EXPECT_THROW(idx.add("j1"), std::invalid_argument);
}

TEST(NameIndex, PopBackAndRemoveAtStayConsistent) {
    NameIndex idx;
    idx.add("A1");
    idx.add("B2");
    idx.add("C3");

    idx.pop_back();
    EXPECT_EQ(idx.size(), 2);
    EXPECT_EQ(idx.find("c3"), -1);
    EXPECT_EQ(idx.try_add("c3"), 2);  // slot reusable after pop

    idx.remove_at(0);  // drop A1; B2 and c3 shift down
    EXPECT_EQ(idx.find("a1"), -1);
    EXPECT_EQ(idx.find("b2"), 0);
    EXPECT_EQ(idx.find("C3"), 1);
}

TEST(NameIndex, RenameRejectsCollisionButAllowsCaseRespelling) {
    NameIndex idx;
    idx.add("T1");
    idx.add("T2");

    // Case-variant collision with a DIFFERENT entry is rejected.
    EXPECT_FALSE(idx.rename(0, "t2"));
    EXPECT_EQ(idx.name_of(0), "T1");

    // A pure case-respelling of the SAME entry is allowed.
    EXPECT_TRUE(idx.rename(0, "t1"));
    EXPECT_EQ(idx.name_of(0), "t1");
    EXPECT_EQ(idx.find("T1"), 0);

    // A genuinely new name still works, and the old name is released.
    EXPECT_TRUE(idx.rename(0, "T9"));
    EXPECT_EQ(idx.find("t1"), -1);
    EXPECT_EQ(idx.find("t9"), 0);
}
