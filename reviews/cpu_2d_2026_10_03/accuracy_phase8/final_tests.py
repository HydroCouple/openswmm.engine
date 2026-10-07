from pathlib import Path
p=Path(__file__).resolve().parent;s=(p/'test_sources/test_2d_cpu_correctness.cpp').read_text();(p/'prototype_correctness.cpp').write_text(s)
s=s[:s.index('\n\nTEST(Cpu2DCorrectness,SecondOrderShorelineKeepsConnectedWetFaceAccuracy)')]
s+='''

TEST(Cpu2DCorrectness,SecondOrderDistortedThackerKeepsShorelineVelocityBounded) {
    constexpr int n = 32;
    constexpr double dx = 4.0 / n, pi = 3.14159265358979323846;
    const double omega = std::sqrt(8 * inertial::kGravity * .1);
    const double period = 2 * pi / omega, amplitude = (1-.8*.8)/(1+.8*.8);
    const double root = std::sqrt(1-amplitude*amplitude);
    // Maximize the analytical radial speed over the wet disk and a period.
    const double c = (1-root)/amplitude;
    const double exact_max_speed = .5*omega*amplitude*
        std::sqrt((1-c*c)/((1-amplitude*c)*root));
    for (int shape : {0, 1, 2}) { // triangles, quadrilaterals, mixed cells
        SCOPED_TRACE(shape);
        MeshData m; m.resize_vertices((n+1)*(n+1));
        for (int y = 0; y <= n; ++y) for (int x = 0; x <= n; ++x) {
            const int i = y*(n+1)+x;
            const bool inner = x>0 && x<n && y>0 && y<n;
            m.vx[i] = x*dx + (inner ? .15*dx*std::sin(.7*x+1.1*y) : 0);
            m.vy[i] = y*dx + (inner ? .15*dx*std::cos(1.3*x-.4*y) : 0);
            const double xc = m.vx[i]-2, yc = m.vy[i]-2;
            m.vz[i] = .1*(xc*xc+yc*yc-1);
        }
        int nc = 0;
        for (int y=0; y<n; ++y) for (int x=0; x<n; ++x)
            nc += (shape==1 || (shape==2 && (x+y)%2)) ? 1 : 2;
        m.resize_triangles(nc); int cell=0;
        for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
            const int a=y*(n+1)+x, b=a+1, c=a+n+1, d=c+1;
            if (shape==1 || (shape==2 && (x+y)%2)) m.set_quad(cell++,a,b,d,c);
            else {m.set_triangle(cell++,a,b,d); m.set_triangle(cell++,a,d,c);}
        }
        buildMeshTopology(m); ASSERT_TRUE(validateMesh(m).empty());
        auto s = state(m,0); auto o = options(1,1);
        o.momentum=Momentum2D::FULL_SWE; o.reconstruction_order=2;
        o.max_timestep=.25; o.dry_depth=1e-7; o.h_move=1e-6;
        const double scale = root/(1-amplitude);
        for (int i=0; i<m.n_cells(); ++i) {
            const double x=m.tri_cx[i]-2, y=m.tri_cy[i]-2;
            s.volume[i]=std::max(0.0,.1*(scale-(x*x+y*y)*scale*scale))*m.tri_area[i];
            m.mannings_n[i]=0;
        }
        const double initial_volume=sum(s.volume);
        ExplicitInertialSolver solver; solver.initialize(m,s,o);
        double max_speed=0;
        for (int k=0; k<192; ++k) {
            solver.advance(k*period/64,(k+1)*period/64);
            for (int i=0; i<m.n_cells(); ++i) {
                const double speed=ExplicitInertialSolverTestAccess::speed(solver,i);
                ASSERT_TRUE(std::isfinite(speed));
                ASSERT_GE(s.volume[i],0);
                max_speed=std::max(max_speed,speed);
            }
        }
        EXPECT_NEAR(sum(s.volume),initial_volume,1e-12);
        // Generous truncation allowance: the rejected shoreline fit reached
        // 18–52 m/s here despite good depth error and exact conservation.
        EXPECT_LT(max_speed,5*exact_max_speed);
    }
}
'''
(p/'test_sources/test_2d_cpu_correctness.cpp').write_text(s)
