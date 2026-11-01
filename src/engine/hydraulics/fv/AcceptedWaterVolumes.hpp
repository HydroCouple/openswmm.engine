// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

namespace openswmm::fv {
/// Optional accepted-window water observations. Native ft3; directions are
/// kept separately so a reversing face cannot cancel a real transport path.
/// Cell lateral/loss rows are signed source / nonnegative loss, respectively.
/// A node deficit records a floored bucket's unfulfilled net removal; it is
/// not automatically classified as an accepted external withdrawal.
struct AcceptedWaterVolumes {
    std::vector<double> face_forward, face_reverse;
    std::vector<double> structure_forward, structure_reverse;
    std::vector<double> cell_lateral, cell_loss, node_deficit;
    std::vector<double> face_lateral_left, face_lateral_right, face_spill;

    void resize(std::size_t nf, std::size_t ns, std::size_t nc, std::size_t nn) {
        face_forward.assign(nf, 0.0); face_reverse.assign(nf, 0.0);
        structure_forward.assign(ns, 0.0); structure_reverse.assign(ns, 0.0);
        cell_lateral.assign(nc, 0.0); cell_loss.assign(nc, 0.0);
        node_deficit.assign(nn, 0.0);
        face_lateral_left.assign(nf,0.0); face_lateral_right.assign(nf,0.0); face_spill.assign(nf,0.0);
    }
    template<class Fn> void each(Fn fn) {
        fn(face_forward); fn(face_reverse); fn(structure_forward); fn(structure_reverse);
        fn(cell_lateral); fn(cell_loss); fn(node_deficit);
        fn(face_lateral_left); fn(face_lateral_right); fn(face_spill);
    }
    void clear() { each([](auto& v) { std::fill(v.begin(), v.end(), 0.0); }); }
    void averageDelta(const AcceptedWaterVolumes& base) {
        auto avg=[](auto& v, const auto& b) {
            for (std::size_t i=0;i<v.size();++i) v[i]=b[i]+0.5*(v[i]-b[i]);
        };
        avg(face_forward,base.face_forward); avg(face_reverse,base.face_reverse);
        avg(structure_forward,base.structure_forward); avg(structure_reverse,base.structure_reverse);
        avg(cell_lateral,base.cell_lateral); avg(cell_loss,base.cell_loss);
        avg(node_deficit,base.node_deficit);
        avg(face_lateral_left,base.face_lateral_left); avg(face_lateral_right,base.face_lateral_right);
        avg(face_spill,base.face_spill);
    }
    void face(std::size_t f, double volume) {
        if (volume >= 0) face_forward[f] += volume; else face_reverse[f] -= volume;
    }
    void structure(std::size_t s, double volume) {
        if (volume >= 0) structure_forward[s] += volume; else structure_reverse[s] -= volume;
    }
};
} // namespace openswmm::fv
