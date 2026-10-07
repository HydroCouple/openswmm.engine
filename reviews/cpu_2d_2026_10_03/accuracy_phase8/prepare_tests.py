from pathlib import Path
p=Path(__file__).resolve().parent;root=p.parents[2];s=(root/'tests/unit/engine/test_2d_cpu_correctness.cpp').read_text()
s=s.replace('    static void fireSwe(','''    static double speed(const ExplicitInertialSolver& s,int i) {
        const double h = s.state_->depth[i];
        return h > s.opts_->dry_depth ? std::hypot(s.qcx_[i],s.qcy_[i])/h : 0.0;
    }
    static void fireSwe(''',1)
s=s.replace('        for (int k = 0; k < 192; ++k) solver.advance(k * period / 64, (k + 1) * period / 64);','''        double max_speed = 0;
        for (int k = 0; k < 192; ++k) {
            solver.advance(k * period / 64, (k + 1) * period / 64);
            for (int i = 0; i < m.n_cells(); ++i) {
                const double speed = ExplicitInertialSolverTestAccess::speed(solver, i);
                ASSERT_TRUE(std::isfinite(speed));
                max_speed = std::max(max_speed, speed);
            }
        }
        // The analytical velocity is uniform, with magnitude .5*omega.
        // Allow shoreline truncation error, but catch thin-film acceleration
        // even when total volume, centroid phase and depth error look good.
        EXPECT_LT(max_speed, 5 * .5 * omega) << "quad=" << quad;''')
s+='''

TEST(Cpu2DCorrectness,SecondOrderShorelineKeepsConnectedWetFaceAccuracy) {
    using A = ExplicitInertialSolverTestAccess;
    for (bool quad : {false, true}) for (int threads : {1, 4}) {
        auto m = grid(8, quad);
        for (int i = 0; i < m.n_vertices(); ++i)
            m.vz[i] = .04 * m.vx[i] + .03 * m.vy[i];
        buildMeshTopology(m); auto s = state(m, 0); auto o = options(threads, 1);
        o.momentum = Momentum2D::FULL_SWE; o.reconstruction_order = 2;
        for (int i = 0; i < m.n_cells(); ++i) {
            s.volume[i] = m.tri_cx[i] < 4 ? .5 * m.tri_area[i] : 0;
            m.tri_init_u[i] = .2; m.tri_init_v[i] = -.1; m.mannings_n[i] = 0;
        }
        ExplicitInertialSolver solver; solver.initialize(m, s, o);
        A::rebuild(solver); A::fireSwe(solver, 1e-4);
        const auto& ed = A::edges(solver); int checked = 0;
        auto shore = [&](int c) {
            for (int p = ed.cell_ptr[c]; p < ed.cell_ptr[c+1]; ++p) {
                const int e = ed.cell_edge[p], j = ed.cL[e] == c ? ed.cR[e] : ed.cL[e];
                if (s.depth[j] == 0) return true;
            }
            return false;
        };
        auto interior = [&](int c) {
            return m.tri_cx[c] > 1 && m.tri_cx[c] < 7 &&
                   m.tri_cy[c] > 1 && m.tri_cy[c] < 7;
        };
        for (int e : A::faces(solver)) {
            const int a = ed.cL[e], b = ed.cR[e];
            if (!interior(a) || !interior(b) || s.depth[a] == 0 || s.depth[b] == 0) continue;
            if (!shore(a) && !shore(b)) continue;
            // The dry neighbor must not suppress a linear, fully wet face
            // reconstruction when two independent wet directions remain.
            EXPECT_NEAR(A::discharge(solver,e), .5*(.2*ed.nx[e]-.1*ed.ny[e]), 1e-13);
            ++checked;
        }
        EXPECT_GT(checked, 3);
    }
}
'''
(p/'test_sources').mkdir(exist_ok=True);(p/'test_sources/test_2d_cpu_correctness.cpp').write_text(s)
