// SPDX-License-Identifier: Apache-2.0
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/gpu/ExplicitKokkosSurfaceSolver.hpp"
#include "2d/gpu/KokkosSweKernels.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialKernels.hpp"
#include <cmath>
#include <iostream>
#include <numeric>
#include <stdexcept>
using namespace openswmm::twoD;
namespace gpu = openswmm::twoD::gpu;
namespace sk = gpu::swe_kernels;
namespace openswmm::twoD {
struct ExplicitInertialSolverTestAccess {
    static void gradients(ExplicitInertialSolver &s) { s.computeLimitedGradientsSwe(); }
    static void faces(ExplicitInertialSolver &s, double dt) {
        s.fireFacesSwe(s.active_faces_, dt, true);
    }
    static void cells(ExplicitInertialSolver &s, double dt) {
        s.fireCells(s.active_cells_, dt, true);
    }
    static auto &edges(ExplicitInertialSolver &s) { return s.edges_; }
    static auto &cells(ExplicitInertialSolver &s) { return s.active_cells_; }
    static auto &faceList(ExplicitInertialSolver &s) { return s.active_faces_; }
    static auto &active(ExplicitInertialSolver &s) { return s.cell_active_; }
    static auto &tiers(ExplicitInertialSolver &s) { return s.tier_; }
    static auto &qx(ExplicitInertialSolver &s) { return s.qcx_; }
    static auto &qy(ExplicitInertialSolver &s) { return s.qcy_; }
    static std::vector<double> gradients(ExplicitInertialSolver &s, int axis) {
        std::vector<double> v;
        const auto n = s.state_->depth.size();
        auto &eta = axis ? s.gey_ : s.gex_;
        auto &u = axis ? s.guy_ : s.gux_;
        auto &w = axis ? s.gvy_ : s.gvx_;
        v.insert(v.end(), eta.begin(), eta.begin() + n);
        v.insert(v.end(), u.begin(), u.end());
        v.insert(v.end(), w.begin(), w.end());
        v.insert(v.end(), eta.begin() + n, eta.end());
        return v;
    }
    static std::vector<std::vector<double>> transfers(ExplicitInertialSolver &s) {
        return {s.facc_L_, s.facc_R_, s.macc_x_L_, s.macc_x_R_, s.macc_y_L_, s.macc_y_R_, s.q_};
    }
};
} // namespace openswmm::twoD
namespace openswmm::twoD::gpu {
struct ExplicitKokkosSurfaceSolverTestAccess {
    static int coarse(ExplicitKokkosSurfaceSolver &s) {
        s.pushForcings();
        s.syncAndRebuild(0.0);
        auto active = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, s.d_active_);
        auto tiers = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, s.d_tier_);
        for (int i = 0; i < int(tiers.extent(0)); ++i)
            if (active(i) && tiers(i) > 0)
                return i;
        return -1;
    }
    static double checkBound(ExplicitKokkosSurfaceSolver &s, int i, double depth) {
        auto h = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, s.d_depth_);
        h(i) = depth;
        Kokkos::deep_copy(s.d_depth_, h);
        s.refreshDt0();
        auto tiers = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, s.d_tier_);
        const double bound =
            inertial::cellCflDt(s.opts_->cfl_number, s.edges_.cell_lchar[i], depth, 0.0);
        return s.dt0_ * (1 << tiers(i)) / bound;
    }
};
} // namespace openswmm::twoD::gpu
using A = ExplicitInertialSolverTestAccess;
void require(bool yes, const char *what) {
    if (!yes)
        throw std::runtime_error(what);
}
void near(double a, double b, const char *what, double tol = 2e-11) {
    if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a - b) > tol * (1 + std::abs(b))) {
        std::cerr << what << ": " << a << " != " << b << '\n';
        throw std::runtime_error(what);
    }
}
template <class T> auto device(const std::vector<T> &data) {
    Kokkos::View<T *, gpu::MemSpace> d("test", data.size());
    auto h = Kokkos::create_mirror_view(d);
    for (size_t i = 0; i < data.size(); ++i)
        h(i) = data[i];
    Kokkos::deep_copy(d, h);
    return d;
}
template <class V> auto host(const V &d) {
    auto h = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, d);
    std::vector<typename V::non_const_value_type> v(d.extent(0));
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = h(i);
    return v;
}
void compare(const std::vector<double> &a, const std::vector<double> &b, const char *what,
             double tol = 2e-11) {
    require(a.size() == b.size(), "size");
    for (size_t i = 0; i < a.size(); ++i)
        near(a[i], b[i], what, tol);
}
double sum(const std::vector<double> &v) { return std::accumulate(v.begin(), v.end(), 0.0); }
MeshData mesh(int n, int shape = 0, bool skew = false, bool slope = false) {
    MeshData m;
    m.resize_vertices((n + 1) * (n + 1));
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) {
            int v = y * (n + 1) + x;
            m.vx[v] = x;
            m.vy[v] = y;
            if (slope)
                m.vz[v] = .015 * x + .007 * y;
            if (skew && x > 0 && x < n && y > 0 && y < n) {
                m.vx[v] += .15 * std::sin(v);
                m.vy[v] += .15 * std::cos(v);
            }
        }
    int count = 0;
    for (int j = 0; j < n * n; ++j)
        count += shape == 1 || (shape == 2 && j % 2) ? 1 : 2;
    m.resize_triangles(count);
    int c = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            int a = y * (n + 1) + x, b = a + 1, d = a + n + 1, e = d + 1;
            if (shape == 1 || (shape == 2 && (y * n + x) % 2))
                m.set_quad(c++, a, b, e, d);
            else {
                m.set_triangle(c++, a, b, e);
                m.set_triangle(c++, a, e, d);
            }
        }
    buildMeshTopology(m);
    return m;
}
SurfaceStateData state(const MeshData &m, double depth) {
    SurfaceStateData s;
    s.resize(m.n_cells(), m.n_vertices());
    for (int i = 0; i < m.n_cells(); ++i)
        s.volume[i] = depth * m.tri_area[i];
    return s;
}
SolverOptions2D options(int tiers = 1) {
    SolverOptions2D o;
    o.lts_tiers = tiers;
    o.max_timestep = 1;
    o.cfl_number = .4;
    o.num_threads = 1;
    return o;
}
void frozenTier() {
    auto m = mesh(16);
    auto s = state(m, .0041);
    auto o = options(4);
    for (int i = 0; i < m.n_cells(); ++i)
        if (m.tri_cx[i] < 4)
            s.volume[i] = .3 * m.tri_area[i];
    gpu::ExplicitKokkosSurfaceSolver solver;
    solver.initialize(m, s, o);
    using Access = gpu::ExplicitKokkosSurfaceSolverTestAccess;
    int i = Access::coarse(solver);
    require(i >= 0, "coarse CFL fixture");
    require(Access::checkBound(solver, i, .2) <= 1 + 1e-14, "frozen-tier CFL bound");
}
void unsupported() {
    for (int kind = 0; kind < 3; ++kind) {
        auto m = mesh(2, kind == 0 ? 1 : 0);
        auto s = state(m, .1);
        auto o = options();
        if (kind == 1)
            o.momentum = Momentum2D::FULL_SWE;
        if (kind == 2)
            s.transport.resize(1, m.n_cells(), 0);
        bool rejected = false;
        try {
            gpu::ExplicitKokkosSurfaceSolver solver;
            solver.initialize(m, s, o);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        require(rejected, "unsupported direct Kokkos initialization");
    }
}
void marcher() {
    for (bool vfr : {false, true})
        for (int tiers : {1, 4})
            for (int scenario = 0; scenario < 5; ++scenario) {
                auto m = mesh(12, 0, false, vfr);
                auto s = state(m, .1);
                auto o = options(tiers);
                if (vfr)
                    o.cell_closure = CellClosure2D::VFR;
                for (int i = 0; i < m.n_cells(); ++i) {
                    if (scenario == 0)
                        s.volume[i] = (m.tri_cx[i] < 6 ? .2 : 0) * m.tri_area[i];
                    if (scenario == 1) {
                        s.infil_rate[i] = .15;
                        s.evap_rate[i] = .1;
                        s.coupling_flux[i] = -.2;
                    }
                    if (scenario == 2) {
                        s.volume[i] = 0;
                        s.rainfall[i] = .02;
                    }
                    if (scenario == 3) {
                        s.rainfall[i] = .1;
                        s.infil_rate[i] = .1;
                    }
                    if (scenario == 4) {
                        s.volume[i] = (m.tri_cx[i] < 6 ? .2 : 0) * m.tri_area[i];
                        s.infil_rate[i] = .2;
                    }
                }
                auto ref = s;
                auto ro = o;
                ExplicitInertialSolver cpu;
                gpu::ExplicitKokkosSurfaceSolver kk;
                cpu.initialize(m, ref, ro);
                kk.initialize(m, s, o);
                double t = 0;
                for (double end : {.93282250995272253, 1.8656450199054451, 2.0}) {
                    cpu.advance(t, end);
                    kk.advance(t, end);
                    std::cout << "step tiers=" << tiers << " case=" << scenario << " end=" << end
                              << " cpu=" << cpu.last_num_steps() << " kk=" << kk.last_num_steps()
                              << std::endl;
                    compare(s.volume, ref.volume, "marcher volume", 2e-9);
                    compare(s.infil_applied, ref.infil_applied, "infiltration", 2e-9);
                    compare(s.coupling_applied, ref.coupling_applied, "coupling", 2e-9);
                    near(s.evap_loss_total, ref.evap_loss_total, "evaporation", 2e-9);
                    if (scenario == 0)
                        near(sum(s.volume), .2 * 72, "closed volume");
                    if (scenario == 1) {
                        double infiltrated = 0;
                        for (int i = 0; i < m.n_cells(); ++i)
                            infiltrated += s.infil_applied[i] * m.tri_area[i];
                        near(sum(s.volume) + infiltrated + s.evap_loss_total -
                                 sum(s.coupling_applied),
                             14.4, "source budget conservation");
                    }
                    if (scenario == 2)
                        near(sum(s.volume), .02 * 144 * end, "rain storage");
                    t = end;
                }
                std::cout << "marcher tiers=" << tiers << " scenario=" << scenario << " PASS\n";
            }
}
sk::Mesh mirrorMesh(MeshData &m, InertialEdges &e) {
    sk::Mesh d;
    std::vector<int> nv(m.n_cells()), nei(4 * m.n_cells(), -1),
        sign(e.cell_sign.begin(), e.cell_sign.end());
    std::vector<double> ax(nei.size()), ay(nei.size()), conv(e.ne);
    for (int i = 0; i < m.n_cells(); ++i) {
        nv[i] = m.cell_vertex_count(i);
        for (int k = 0; k < nv[i]; ++k) {
            int slot = 4 * i + k;
            nei[slot] = m.cell_neighbour(i, k);
            int a = m.cell_vertex(i, k), b = m.cell_vertex(i, (k + 1) % nv[i]);
            ax[slot] = .5 * (m.vx[a] + m.vx[b]) - m.tri_cx[i];
            ay[slot] = .5 * (m.vy[a] + m.vy[b]) - m.tri_cy[i];
        }
    }
    for (int k = 0; k < e.ne; ++k)
        conv[k] = m.edge_conveyance[e.slotL[k]];
    d.nv = device(nv);
    d.neighbour = device(nei);
    d.cell_ptr = device(e.cell_ptr);
    d.cell_edge = device(e.cell_edge);
    d.cell_sign = device(sign);
    d.cL = device(e.cL);
    d.cR = device(e.cR);
    d.cx = device(m.tri_cx);
    d.cy = device(m.tri_cy);
    d.area = device(m.tri_area);
    d.nx = device(e.nx);
    d.ny = device(e.ny);
    d.xi = device(e.xi);
    d.mx = device(e.mx);
    d.my = device(e.my);
    d.conveyance = device(conv);
    d.cell_arm_x = device(e.cell_arm_x);
    d.cell_arm_y = device(e.cell_arm_y);
    d.slot_arm_x = device(ax);
    d.slot_arm_y = device(ay);
    d.boundary_nx = device(m.edge_nx);
    d.boundary_ny = device(m.edge_ny);
    d.boundary_length = device(m.edge_length);
    return d;
}
void sweChecks() {
    for (int shape : {0, 1, 2})
        for (int scenario = 0; scenario < 4; ++scenario)
            for (int order : {1, 2}) {
                auto m = mesh(6, shape, true);
                if (scenario == 3)
                    for (double &c : m.edge_conveyance)
                        c = .35;
                auto s = state(m, .3);
                auto o = options();
                o.momentum = Momentum2D::FULL_SWE;
                o.reconstruction_order = order;
                o.dry_depth = 1e-5;
                for (int i = 0; i < m.n_cells(); ++i) {
                    s.volume[i] = (.12 + .03 * m.tri_cx[i]) * m.tri_area[i];
                    if (scenario > 0 && m.tri_cx[i] > 4)
                        s.volume[i] = scenario == 2 ? 1e-7 * m.tri_area[i] : 0;
                }
                ExplicitInertialSolver cpu;
                cpu.initialize(m, s, o);
                for (int i = 0; i < m.n_cells(); ++i) {
                    A::qx(cpu)[i] = s.depth[i] * (.1 + .02 * m.tri_cy[i]);
                    A::qy(cpu)[i] = s.depth[i] * (-.03 + .01 * m.tri_cx[i]);
                    if (scenario == 3 && m.tri_cx[i] > 3)
                        s.head[i] += 1;
                }
                auto &e = A::edges(cpu);
                auto dm = mirrorMesh(m, e);
                std::vector<int> active(A::active(cpu).begin(), A::active(cpu).end());
                sk::State ds{device(s.head),
                             device(s.depth),
                             device(s.volume),
                             device(A::qx(cpu)),
                             device(A::qy(cpu)),
                             device(active),
                             device(std::vector<int>(A::tiers(cpu).begin(), A::tiers(cpu).end()))};
                sk::Gradients g{gpu::DView("gx", 4 * m.n_cells()),
                                gpu::DView("gy", 4 * m.n_cells())};
                auto cells = device(A::cells(cpu));
                auto faces = device(A::faceList(cpu));
                gpu::IView tiers("face_tiers", e.ne);
                if (order == 2) {
                    A::gradients(cpu);
                    sk::gradients(dm, ds, g, cells, A::cells(cpu).size(), o.dry_depth);
                    compare(host(g.x), A::gradients(cpu, 0), "gradient x");
                    compare(host(g.y), A::gradients(cpu, 1), "gradient y");
                }
                sk::Transfers tr{gpu::DView("wl", e.ne), gpu::DView("wr", e.ne),
                                 gpu::DView("xl", e.ne), gpu::DView("xr", e.ne),
                                 gpu::DView("yl", e.ne), gpu::DView("yr", e.ne),
                                 gpu::DView("q", e.ne)};
                double dt = scenario == 2 ? 10. : .03;
                A::faces(cpu, dt);
                sk::faces(dm, ds, g, tr, faces, A::faceList(cpu).size(), tiers, dt, o.dry_depth,
                          o.exchange_beta, order == 2, true);
                auto expected = A::transfers(cpu);
                int k = 0;
                for (auto v : {tr.waterL, tr.waterR, tr.mxL, tr.mxR, tr.myL, tr.myR, tr.discharge})
                    compare(host(v), expected[k++], "face transfer");
                near(sum(host(tr.waterL)) + sum(host(tr.waterR)), 0, "face conservation");
                gpu::DView dx("dx", m.n_cells()), dy("dy", m.n_cells());
                sk::gather(dm, ds, tr, cells, A::cells(cpu).size(), dx, dy);
                std::vector<double> ev = s.volume, ex(m.n_cells()), ey(m.n_cells());
                for (int i : A::cells(cpu)) {
                    double dv = 0;
                    for (int p = e.cell_ptr[i]; p < e.cell_ptr[i + 1]; ++p) {
                        int edge = e.cell_edge[p], side = e.cell_sign[p] > 0 ? 0 : 1;
                        dv += expected[side][edge];
                        ex[i] += expected[2 + side][edge];
                        ey[i] += expected[4 + side][edge];
                    }
                    ev[i] = std::max(0., ev[i] + dv);
                }
                compare(host(ds.volume), ev, "gather volume");
                compare(host(dx), ex, "gather mx");
                compare(host(dy), ey, "gather my");
                for (auto v : {tr.waterL, tr.waterR, tr.mxL, tr.mxR, tr.myL, tr.myR})
                    near(sum(host(v)), 0, "consumed transfers");
                // Complete the source-free Euler cell operation: FLAT closure followed
                // by wall pressure and friction. Compare the integrated CPU cell routine.
                A::cells(cpu, dt);
                std::vector<double> depth = ev, head = ev;
                for (int i = 0; i < m.n_cells(); ++i) {
                    depth[i] = ev[i] / m.tri_area[i];
                    head[i] = m.tri_cz[i] + depth[i];
                }
                Kokkos::deep_copy(ds.depth, device(depth));
                Kokkos::deep_copy(ds.head, device(head));
                std::vector<int> wallptr(m.n_cells() + 1), wallslots;
                for (int i = 0; i < m.n_cells(); ++i) {
                    wallptr[i] = wallslots.size();
                    for (int k = 0; k < m.cell_vertex_count(i); ++k)
                        if (m.cell_neighbour(i, k) < 0)
                            wallslots.push_back(4 * i + k);
                }
                wallptr.back() = wallslots.size();
                auto wall_ptr = device(wallptr);
                auto wall_slot = device(wallslots);
                auto roughness = device(m.mannings_n);
                sk::momentum(dm, ds, cells, A::cells(cpu).size(), dx, dy, wall_ptr, wall_slot,
                             roughness, dt, o.dry_depth);
                compare(host(ds.volume), s.volume, "Euler volume");
                compare(host(ds.qx), A::qx(cpu), "Euler momentum x");
                compare(host(ds.qy), A::qy(cpu), "Euler momentum y");
                std::cout << "SWE shape=" << shape << " scenario=" << scenario << " order=" << order
                          << " PASS\n";
            }
}
void externalVolumeResync() {
    auto m = mesh(4);
    auto a = state(m, 0.0), b = a;
    auto o = options(4);
    ExplicitInertialSolver cpu;
    gpu::ExplicitKokkosSurfaceSolver device_solver;
    cpu.initialize(m, a, o); device_solver.initialize(m, b, o);
    cpu.advance(0, 1); device_solver.advance(0, 1);
    a.volume[0] = b.volume[0] = 1.0;
    cpu.resyncFromVolumes(1); device_solver.resyncFromVolumes(1);
    cpu.advance(1, 2); device_solver.advance(1, 2);
    near(sum(a.volume), 1.0, "CPU external transfer conservation");
    near(sum(b.volume), 1.0, "Kokkos external transfer conservation");
    require(a.volume[0] < 1.0 && b.volume[0] < 1.0, "external transfer activates faces");
    compare(a.volume, b.volume, "external transfer CPU/Kokkos parity", 2e-9);
    std::cout << "External volume resync PASS\n";
}
int main(int argc, char **argv) {
    Kokkos::initialize(argc, argv);
    std::cout << "Execution space: " << gpu::ExecSpace::name()
              << ", concurrency=" << gpu::ExecSpace().concurrency() << std::endl;
    int rc = 0;
    try {
        frozenTier();
        unsupported();
        marcher();
        externalVolumeResync();
        sweChecks();
        auto initial = device(std::vector<double>{7., -3., 0.});
        auto live = device(std::vector<double>{11., 5., 2.});
        sk::averageLedger(initial, live);
        compare(host(live), {9., 1., 1.}, "RK ledger average");
        std::cout << "PASS host/device-space alignment checks\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        rc = 1;
    }
    Kokkos::finalize();
    return rc;
}
