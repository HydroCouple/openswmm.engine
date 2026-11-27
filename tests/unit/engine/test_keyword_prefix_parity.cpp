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
 * @file      test_keyword_prefix_parity.cpp
 * @brief     Issue #96: keywords are read as legacy findmatch() reads them.
 *
 * @details Each case in data/keyword_prefix/cases.tsv edits one deck once and states what EPA
 *          SWMM 5.2.4 does with the edit: `same` (the same binary output as the unedited deck) or
 *          `refuse:NNN` (the run stops with ERROR NNN). The table is checked against 5.2.4 and
 *          OpenSWMM 5.3.0 by tests/verification/name_keyword_case_96_2026-11-26/oracle_cases.py;
 *          this test holds the 6.0 engine to it. Decks, reports and outputs are written to
 *          data/keyword_prefix_out/.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

namespace {

namespace fs = std::filesystem;

const fs::path kData = fs::path(KEYWORD_PREFIX_DATA_DIR);
const fs::path kOut = fs::path(KEYWORD_PREFIX_OUT_DIR);

struct Case {
    std::string id, deck, find, replace, expect;
};

// gtest prints a failing parameter with this, instead of its bytes.
void PrintTo(const Case& c, std::ostream* os) {
    *os << c.id << " (" << c.deck << ", expect " << c.expect << ")";
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == 'n' || s[i + 1] == 't')) {
            out += s[i + 1] == 'n' ? '\n' : '\t';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<Case> load_cases() {
    std::vector<Case> cases;
    std::istringstream in(read_file(kData / "cases.tsv"));
    std::string line;
    while (std::getline(in, line)) {
        // A Windows checkout gives the file CRLF endings; the last column would
        // otherwise read "same\r" and take the refuse branch.
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> col;
        std::size_t start = 0;
        for (std::size_t tab; (tab = line.find('\t', start)) != std::string::npos; start = tab + 1)
            col.push_back(line.substr(start, tab - start));
        col.push_back(line.substr(start));
        if (col.size() == 5)
            cases.push_back({col[0], col[1], unescape(col[2]), unescape(col[3]), col[4]});
    }
    return cases;
}

struct DeckRun {
    int rc = 0;
    std::string rpt, out;
};

// Open and, when the open succeeds, run the full lifecycle; rc is the first failing call's code.
DeckRun run_deck(const fs::path& inp) {
    const fs::path rpt = fs::path(inp).replace_extension(".rpt");
    const fs::path out = fs::path(inp).replace_extension(".out");
    fs::remove(rpt);
    fs::remove(out);
    DeckRun r;
    SWMM_Engine e = swmm_engine_create();
    r.rc = swmm_engine_open(e, inp.string().c_str(), rpt.string().c_str(), out.string().c_str(),
                            nullptr);
    if (r.rc == SWMM_OK)
        r.rc = swmm_engine_initialize(e);
    if (r.rc == SWMM_OK)
        r.rc = swmm_engine_start(e, 1);
    if (r.rc == SWMM_OK) {
        double elapsed = 0.0;
        int rc = SWMM_OK;
        while ((rc = swmm_engine_step(e, &elapsed)) == SWMM_OK && elapsed > 0.0) {
        }
        r.rc = rc;
        swmm_engine_end(e);
        swmm_engine_report(e);
    }
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    r.rpt = read_file(rpt);
    r.out = read_file(out);
    return r;
}

// The unedited deck's run, once per deck.
const DeckRun& base_run(const std::string& deck) {
    static std::map<std::string, DeckRun> runs;
    auto it = runs.find(deck);
    if (it == runs.end()) {
        fs::create_directories(kOut);
        const fs::path inp = kOut / ("base__" + deck);
        std::ofstream(inp, std::ios::binary) << read_file(kData / deck);
        it = runs.emplace(deck, run_deck(inp)).first;
    }
    return it->second;
}

class KeywordPrefixParity : public ::testing::TestWithParam<Case> {};

TEST_P(KeywordPrefixParity, ReadsAsLegacyDoes) {
    const Case& c = GetParam();
    const DeckRun& base = base_run(c.deck);
    ASSERT_EQ(base.rc, SWMM_OK) << c.deck << " does not run cleanly:\n" << base.rpt;
    ASSERT_EQ(base.rpt.find("ERROR"), std::string::npos) << base.rpt;

    std::string text = read_file(kData / c.deck);
    const std::size_t at = text.find(c.find);
    ASSERT_NE(at, std::string::npos) << "find text not in " << c.deck;
    ASSERT_EQ(text.find(c.find, at + 1), std::string::npos) << "find text twice in " << c.deck;
    text.replace(at, c.find.size(), c.replace);
    const fs::path inp = kOut / (c.id + ".inp");
    std::ofstream(inp, std::ios::binary) << text;

    const DeckRun r = run_deck(inp);
    if (c.expect == "same") {
        EXPECT_EQ(r.rc, SWMM_OK) << r.rpt;
        EXPECT_EQ(r.rpt.find("ERROR"), std::string::npos) << r.rpt;
        EXPECT_TRUE(!r.out.empty() && r.out == base.out) << "binary output differs from " << c.deck;
    } else {
        ASSERT_EQ(c.expect.rfind("refuse:", 0), 0u) << "bad expect " << c.expect;
        const std::string code = "ERROR " + c.expect.substr(7);
        EXPECT_NE(r.rc, SWMM_OK);
        EXPECT_NE(r.rpt.find(code), std::string::npos) << "no " << code << " in:\n" << r.rpt;
    }
}

INSTANTIATE_TEST_SUITE_P(Cases, KeywordPrefixParity, ::testing::ValuesIn(load_cases()),
                         [](const ::testing::TestParamInfo<Case>& info) { return info.param.id; });

TEST(KeywordPrefixParity, TableIsNotEmpty) {
    EXPECT_FALSE(load_cases().empty()) << (kData / "cases.tsv").string();
}

} // namespace
