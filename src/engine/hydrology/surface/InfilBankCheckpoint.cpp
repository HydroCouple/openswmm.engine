// SPDX-License-Identifier: Apache-2.0

/**
 * @file      InfilBankCheckpoint.cpp
 * @brief     Checkpoint persistence for surface infiltration state.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "InfilBank.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include <cmath>
namespace openswmm::surface {
void InfilBank::checkpoint(detail::CompletedSourceArchive& a, bool model) {
    if (model) {
        a(methods_, owners_, constant_);
        return;
    }
    a.fixed(cumulative_);
    a.sequence(
        horton_, [&](auto& h) { a(h.f0, h.fmin, h.decay, h.regen, h.Fmax, h.tp, h.Fe, h.Fmh); },
        true);
    a.sequence(
        grnampt_,
        [&](auto& g) { a(g.S, g.Ks, g.IMDmax, g.IMD, g.F, g.Fu, g.Fumax, g.Lu, g.T, g.saturated); },
        true);
    a.sequence(
        curvenum_, [&](auto& c) { a(c.Smax, c.S, c.Se, c.P, c.F, c.f, c.regen, c.T, c.Tmax); },
        true);
}
void InfilBank::validateCheckpoint(const InfilBank& model) const {
    const auto require = [](bool ok) {
        if (!ok)
            throw std::runtime_error("Invalid runoff infiltration checkpoint/material.");
    };
    require(methods_ == model.methods_ && owners_ == model.owners_ &&
            constant_ == model.constant_ && horton_.size() == model.horton_.size() &&
            grnampt_.size() == model.grnampt_.size() && curvenum_.size() == model.curvenum_.size());
    for (double v : cumulative_)
        require(std::isfinite(v) && v >= 0);
    for (std::size_t i = 0; i < horton_.size(); ++i) {
        const auto& h = horton_[i];
        const auto& m = model.horton_[i];
        require(h.f0 == m.f0 && h.fmin == m.fmin && h.decay == m.decay && h.regen == m.regen &&
                h.Fmax == m.Fmax);
        for (double v : {h.f0, h.fmin, h.decay, h.regen, h.Fmax, h.tp, h.Fe, h.Fmh})
            require(std::isfinite(v) && v >= 0);
    }
    for (std::size_t i = 0; i < grnampt_.size(); ++i) {
        const auto& g = grnampt_[i];
        const auto& m = model.grnampt_[i];
        require(g.S == m.S && g.Ks == m.Ks && g.IMDmax == m.IMDmax && g.Fumax == m.Fumax &&
                g.Lu == m.Lu);
        for (double v : {g.S, g.Ks, g.IMDmax, g.IMD, g.F, g.Fu, g.Fumax, g.Lu, g.T})
            require(std::isfinite(v));
        require(g.IMD >= 0 && g.IMD <= g.IMDmax && g.F >= 0 && g.Fu >= 0);
    }
    for (std::size_t i = 0; i < curvenum_.size(); ++i) {
        const auto& c = curvenum_[i];
        const auto& m = model.curvenum_[i];
        require(c.Smax == m.Smax && c.regen == m.regen && c.Tmax == m.Tmax);
        for (double v : {c.Smax, c.S, c.Se, c.P, c.F, c.f, c.regen, c.T, c.Tmax})
            require(std::isfinite(v) && v >= 0);
        require(c.S <= c.Smax && c.Se <= c.Smax);
    }
}

}
