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
 * @file      test_2d_exchange_rubinato_steady.cpp
 * @brief     The WEIR_ORIFICE law against Rubinato et al. (2017) Table 2/3.
 *
 * @details For each of the 36 steady rows the law is evaluated in the row's
 *          regime with unit coefficients on the rig (D_M = 0.24 m, crest
 *          0.478 m above the pipe invert, φ head scaled to the 1/6 rig), which
 *          gives the paper's equation term X (Eqs. 1, 2, 4). Regressing the
 *          measured Q_e on X recovers Table 3's coefficient as the slope — the
 *          paper's own fit, which has an intercept (−2.5 and −3.8 l/s for the
 *          orifice scenarios), so the gate is the slope (±0.01) and R² ≥ 0.96,
 *          not the absolute flow (plan §0R R2). The absolute law-vs-measured
 *          comparison is written to ./rubinato_steady_out/ for review only.
 *
 *          Reference data: tests/benchmarks/rubinato2017/ (provenance there).
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "2d/coupling/ExchangeLaw.hpp"

namespace fs = std::filesystem;
using namespace openswmm::twoD::exchange;

namespace {

const fs::path kRef = fs::path(__FILE__).parent_path() / "../../benchmarks/rubinato2017";
constexpr double kPi = 3.14159265358979323846;
constexpr double kDm = 0.24, kZ = 0.478;

std::vector<std::map<std::string, std::string>> read_csv(const fs::path& p) {
    std::ifstream f(p);
    std::vector<std::map<std::string, std::string>> rows;
    std::string line;
    std::vector<std::string> head;
    auto split = [](const std::string& s) {
        std::vector<std::string> out;
        std::stringstream ss(s);
        for (std::string t; std::getline(ss, t, ',');)
            out.push_back(t);
        if (!s.empty() && s.back() == ',')
            out.emplace_back();
        return out;
    };
    if (!std::getline(f, line))
        return rows;
    head = split(line);
    while (std::getline(f, line)) {
        if (line.empty())
            continue;
        const auto v = split(line);
        std::map<std::string, std::string> r;
        for (std::size_t i = 0; i < head.size(); ++i)
            r[head[i]] = i < v.size() ? v[i] : "";
        rows.push_back(r);
    }
    return rows;
}

// Equation term X of a Table 2 row through the law, unit coefficients.
double term(const std::map<std::string, std::string>& row) {
    const Geometry g{kZ, kPi * kDm, kPi * kDm * kDm / 4.0};
    Coefficients c;
    c.h_eps = 0.02 / 6.0; // φ head at the 1/6 rig scale
    const std::string s = row.at("scenario");
    const double hs = row.at("h_s_m").empty() ? 0.0 : std::stod(row.at("h_s_m"));
    if (s == "S1") { // free weir, orifice not limiting
        c.c_w = 1.0;
        c.c_sw = 2.0 / 3.0;
        c.c_oi = 1e9;
        return exchange(g, c, kZ + hs, kZ - 1.0).q;
    }
    const double hp = std::stod(row.at("h_p_m"));
    if (s == "S2") { // submerged weir: ⅔·C_w = C_sw = 1
        c.c_w = 1.5;
        c.c_sw = 1.0;
        c.c_oi = 1e9;
        return exchange(g, c, kZ + hs, hp).q;
    }
    c.c_oo = 1.0; // S3 wet / S4 dry: sewer → surface orifice
    return -exchange(g, c, kZ + hs, hp).q;
}

} // namespace

TEST(ExchangeRubinatoSteady, LawTermsRecoverTable3Slopes) {
    const auto t2 = read_csv(kRef / "table2_steady_exchange.csv");
    const auto t3 = read_csv(kRef / "table3_coefficients.csv");
    ASSERT_EQ(t2.size(), 36u) << "Table 2 transcription missing at " << kRef;
    ASSERT_EQ(t3.size(), 4u);

    fs::create_directories("rubinato_steady_out");
    std::ofstream rep("rubinato_steady_out/law_vs_table2.csv");
    rep << "scenario,q_e_lps,xi_f_lps,term_m3s,law_q_lps,rel_err\n";

    for (const auto& t : t3) {
        const std::string s = t.at("scenario");
        const double c_expected = std::stod(t.at("c_expected"));
        std::vector<double> x, q;
        for (const auto& r : t2) {
            if (r.at("scenario") != s)
                continue;
            const double xi = term(r);
            const double qe = std::stod(r.at("q_e_lps")) * 1e-3;
            ASSERT_GT(xi, 0.0) << s;
            x.push_back(xi);
            q.push_back(qe);
            rep << s << "," << r.at("q_e_lps") << "," << r.at("xi_f_lps") << "," << xi << ","
                << c_expected * xi * 1e3 << "," << (c_expected * xi - qe) / qe << "\n";
        }
        const double n = static_cast<double>(x.size());
        double mx = 0, mq = 0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            mx += x[i];
            mq += q[i];
        }
        mx /= n;
        mq /= n;
        double sxx = 0, sxq = 0, sqq = 0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            sxx += (x[i] - mx) * (x[i] - mx);
            sxq += (x[i] - mx) * (q[i] - mq);
            sqq += (q[i] - mq) * (q[i] - mq);
        }
        const double slope = sxq / sxx;
        const double r2 = sxq * sxq / (sxx * sqq);
        EXPECT_NEAR(slope, c_expected, 0.01) << s << " regression slope vs Table 3";
        EXPECT_GE(r2, 0.96) << s;
        std::printf("%s: slope %.4f (Table 3 %.3f), intercept %+.2f l/s, R2 %.4f\n", s.c_str(),
                    slope, c_expected, (mq - slope * mx) * 1e3, r2);
    }
}
