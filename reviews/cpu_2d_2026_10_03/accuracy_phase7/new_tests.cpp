
TEST(Cpu2DCorrectness,SecondOrderSlopeReconstructionCarriesPhysicalMassFlux) {
    using A = ExplicitInertialSolverTestAccess;
    for (bool quad : {false, true}) for (int threads : {1, 4}) {
        auto m = grid(8, quad);
        for (int i = 0; i < m.n_vertices(); ++i)
            m.vz[i] = .04 * m.vx[i] + .03 * m.vy[i];
        buildMeshTopology(m);
        auto s = state(m, .5); auto o = options(threads, 1);
        o.momentum = Momentum2D::FULL_SWE; o.reconstruction_order = 2;
        for (int i = 0; i < m.n_cells(); ++i) {
            m.tri_init_u[i] = .2; m.tri_init_v[i] = -.1; m.mannings_n[i] = 0;
        }
        ExplicitInertialSolver solver; solver.initialize(m, s, o);
        A::rebuild(solver); A::fireSwe(solver, 1e-4);
        const auto& ed = A::edges(solver); int checked = 0;
        for (int e : A::faces(solver)) {
            const int a = ed.cL[e], b = ed.cR[e];
            auto interior = [&](int c) {
                return m.tri_cx[c] > 2 && m.tri_cx[c] < 6 &&
                       m.tri_cy[c] > 2 && m.tri_cy[c] < 6;
            };
            if (!interior(a) || !interior(b)) continue;
            // Both reconstructed states have h=.5 and the same velocity.
            // A smooth bed slope must not lower the advected water depth.
            EXPECT_NEAR(A::discharge(solver, e), .5 * (.2 * ed.nx[e] - .1 * ed.ny[e]), 1e-13);
            ++checked;
        }
        EXPECT_GT(checked, 10);
    }
}

TEST(Cpu2DCorrectness,SecondOrderPlanarThackerRetainsRotationPhase) {
    constexpr double pi = 3.14159265358979323846;
    const double omega = std::sqrt(2 * inertial::kGravity * .1);
    const double period = 2 * pi / omega;
    for (bool quad : {false, true}) {
        auto m = grid(64, quad);
        for (int i = 0; i < m.n_vertices(); ++i) {
            m.vx[i] /= 16; m.vy[i] /= 16;
            const double x = m.vx[i] - 2, y = m.vy[i] - 2;
            m.vz[i] = .1 * (x * x + y * y - 1);
        }
        buildMeshTopology(m); auto s = state(m, 0); auto o = options(1, 1);
        o.momentum = Momentum2D::FULL_SWE; o.reconstruction_order = 2;
        o.max_timestep = .25; o.dry_depth = 1e-7; o.h_move = 1e-6;
        for (int i = 0; i < m.n_cells(); ++i) {
            const double x = m.tri_cx[i] - 2.5, y = m.tri_cy[i] - 2;
            s.volume[i] = std::max(0.0, .1 * (1 - x*x - y*y)) * m.tri_area[i];
            m.tri_init_v[i] = .5 * omega; m.mannings_n[i] = 0;
        }
        const double initial_volume = sum(s.volume);
        ExplicitInertialSolver solver; solver.initialize(m, s, o);
        for (int k = 0; k < 192; ++k) solver.advance(k * period / 64, (k + 1) * period / 64);
        double x = 0, y = 0;
        for (int i = 0; i < m.n_cells(); ++i) {
            x += s.volume[i] * (m.tri_cx[i] - 2);
            y += s.volume[i] * (m.tri_cy[i] - 2);
            EXPECT_GE(s.volume[i], 0);
        }
        EXPECT_NEAR(sum(s.volume), initial_volume, 1e-12);
        // Exact centroid returns to (+.5, 0). Allow spatial damping while
        // rejecting the former ~30-degree phase lag after three periods.
        EXPECT_GT(x / initial_volume, .3);
        EXPECT_LT(std::abs(std::atan2(y, x)), .15);
    }
}
