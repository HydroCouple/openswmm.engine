from pathlib import Path
p=Path(__file__).resolve().parent;root=p.parents[2];s=(root/'tests/unit/engine/test_2d_cpu_correctness.cpp').read_text();(p/'baseline_test.cpp').write_text(s)
s+='''

TEST(Cpu2DCorrectness,ReconstructedBedPressureUsesTheCellToFaceDepthIntegral) {
    // For a linear profile, the bed-source integral is -g*mean(h)*delta(z).
    // Both sides agree at the face, so the hydrostatic correction is zero.
    swe::FaceFlux f; double lx,ly,rx,ry;
    const double h_cell=2, h_face=2.2, z_face=.1, eta_face=h_face+z_face;
    ASSERT_TRUE(swe::faceFluxReconBed(eta_face,0,0,0,h_cell,
        eta_face,0,0,z_face,h_face,1,0,1e-7,f,lx,ly,rx,ry,z_face,z_face));
    EXPECT_NEAR(f.mass,0,1e-14);
    EXPECT_NEAR(lx,-inertial::kGravity*.5*(h_cell+h_face)*z_face,1e-14);
    EXPECT_EQ(ly,0); EXPECT_NEAR(rx,0,1e-14); EXPECT_EQ(ry,0);
    // A bounded positive depth polynomial cannot produce a bed-force per
    // unit water volume that diverges as the cell dries.
    for(double h : {1e-4,1e-6,1e-8}) {
        ASSERT_TRUE(swe::faceFluxReconBed(.01+2*h,0,0,0,h,
            .1,0,0,.1,0,1,0,1e-10,f,lx,ly,rx,ry,.01,.1));
        EXPECT_EQ(f.mass,0); EXPECT_EQ(f.mx,0);
        EXPECT_LT(std::abs(lx)/h,.02*inertial::kGravity);
    }
}
'''
# Keep the independently specified wet-face flux regression developed in phase 8.
proto=(p.parent/'accuracy_phase8/prototype_correctness.cpp').read_text();s+='\n'+proto[proto.index('TEST(Cpu2DCorrectness,SecondOrderShorelineKeepsConnectedWetFaceAccuracy)'):]
# Add depth accuracy to the existing three-period planar test; same analytical
# equation and cell quadrature as the separately compiled analytical harness.
a=s.index('TEST(Cpu2DCorrectness,SecondOrderPlanarThackerRetainsRotationPhase)');b=s.index('\nTEST(',a+1);g=s[a:b]
g=g.replace('        double x = 0, y = 0;','        double x = 0, y = 0, depth_error = 0, reference_volume = 0;')
g=g.replace('            EXPECT_GE(s.volume[i], 0);','''            EXPECT_GE(s.volume[i], 0);
            const double dx=m.tri_cx[i]-2.5, dy=m.tri_cy[i]-2;
            const double exact_h=std::max(0.0,.1*(1-dx*dx-dy*dy));
            depth_error += std::abs(s.depth[i]-exact_h)*m.tri_area[i];
            reference_volume += exact_h*m.tri_area[i];''')
g=g.replace('        EXPECT_LT(std::abs(std::atan2(y, x)), .15);','        EXPECT_LT(std::abs(std::atan2(y, x)), .15);\n        EXPECT_LT(depth_error/reference_volume, .18);')
s=s[:a]+g+s[b:];(p/'test_sources').mkdir(exist_ok=True);(p/'test_sources/test_2d_cpu_correctness.cpp').write_text(s)
