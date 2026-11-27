/**
 * @file      PondedDepthExact.cpp
 * @brief     Tables and step of the exact ponded-depth solution (RUNOFF_ODE_SOLVER EXACT).
 *
 * @details Universal functions of du/dtau = sigma - u^m, m = 5/3:
 *          sigma = +1, u < 1:  F(u) = int_0^u ds/(1-s^m)            (tau from u = 0)
 *          sigma = +1, u > 1:  H(u) = int_u^inf ds/(s^m-1)          (tau from u = inf)
 *          sigma = -1:         G(u) = int_0^u ds/(1+s^m)            (time to drain to u = 0),
 *                              Gc(u) = Ginf - G(u)
 *          and their inverses. Each is a quintic Hermite table from (f, f', f'') at the nodes,
 *          all three in closed form from the equation; node values come from 20-point
 *          Gauss-Legendre integration at start-up (~8 ms). The solutions are analytic except at
 *          u = 0 (the u^(m+1) term), so grids that reach small u are geometric in u (or in
 *          tau, which is ~u there) and the outflow there takes a direct u^m rather than the
 *          slope; near u = 1 the grids stop at 2^-40 from equilibrium (where 1 -+ d is still
 *          exact in double) and -(1/m) ln(d) + C takes over. Geometric grids are located with
 *          frexp and an 8-bit mantissa index, uniform ones with a multiply: no search, no
 *          Newton, no transcendental call in the hot path.
 *
 *          Tables (16 nodes per octave, 305 KB):
 *            Fs(u),  u in [2^-60, 1/2]      Fw(w),  w = 1-u in [2^-40, 1/2]
 *            Phis(tau) = u, tau in [2^-60, 1/2]   Phiw(tau) = w, uniform on [1/4, tauF_max]
 *            H(v),   v = u-1 in [2^-40, 2^60]     Psi(tau) = v, tau in [2^-44, 2^5]
 *            Gs(u) = y, u in [2^-60, 1]           Gc(u) = z, u in [1, 2^60]
 *            Gams(y) = u, y in [2^-60, 1]         Gamz(z) = u, z in [2^-60, 1]
 *          with y the time left to drain and z = Ginf - y.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "PondedDepthExact.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace openswmm {
namespace runoff {

namespace {

constexpr double kM = ExactPondedDepth::kExponent;

/// Quintic Hermite segments from (f, f', f'') at the nodes, in the linear position variable.
struct Hermite {
    std::vector<double> pos, f, d1, d2;
    int n = 0;

    inline void eval(int k, double s, double& val, double& dval) const noexcept {
        const double h = pos[k + 1] - pos[k];
        const double f0 = f[k], f1 = f[k + 1];
        const double g0 = d1[k] * h, g1 = d1[k + 1] * h;
        const double q0 = d2[k] * h * h, q1 = d2[k + 1] * h * h;
        const double s2 = s * s, s3 = s2 * s, s4 = s3 * s, s5 = s4 * s;
        val = f0 * (1 - 10 * s3 + 15 * s4 - 6 * s5) + g0 * (s - 6 * s3 + 8 * s4 - 3 * s5) +
              q0 * 0.5 * (s2 - 3 * s3 + 3 * s4 - s5) + f1 * (10 * s3 - 15 * s4 + 6 * s5) +
              g1 * (-4 * s3 + 7 * s4 - 3 * s5) + q1 * 0.5 * (s3 - 2 * s4 + s5);
        dval =
            (f0 * (-30 * s2 + 60 * s3 - 30 * s4) + g0 * (1 - 18 * s2 + 32 * s3 - 15 * s4) +
             q0 * 0.5 * (2 * s - 9 * s2 + 12 * s3 - 5 * s4) + f1 * (30 * s2 - 60 * s3 + 30 * s4) +
             g1 * (-12 * s2 + 28 * s3 - 15 * s4) + q1 * 0.5 * (3 * s2 - 8 * s3 + 5 * s4)) /
            h;
    }
};

/// Nodes at 2^(k/P + e_min), located by frexp and an 8-bit mantissa index.
struct Geo : Hermite {
    int P = 16, e_min = -60;
    double thresh[64];
    unsigned char mant_idx[256];

    void setup(int P_, int e_min_, int e_max) {
        P = P_;
        e_min = e_min_;
        n = (e_max - e_min) * P + 1;
        pos.resize(n);
        f.resize(n);
        d1.resize(n);
        d2.resize(n);
        for (int k = 0; k < n; ++k)
            pos[k] = std::exp2(static_cast<double>(k) / P + e_min);
        for (int j = 0; j < P; ++j)
            thresh[j] = std::exp2(static_cast<double>(j) / P);
        for (int b = 0; b < 256; ++b) {
            const double m2 = 1.0 + b / 256.0;
            int j = 0;
            while (j + 1 < P && thresh[j + 1] <= m2)
                ++j;
            mant_idx[b] = static_cast<unsigned char>(j);
        }
    }
    inline int segment(double p) const noexcept {
        int e;
        const double fr = std::frexp(p, &e);
        const double m2 = 2.0 * fr; // [1, 2)
        std::uint64_t bits;
        std::memcpy(&bits, &m2, sizeof bits);
        int j = mant_idx[(bits >> 44) & 0xFF];
        if (j + 1 < P && thresh[j + 1] <= m2)
            ++j;
        const int k = (e - 1 - e_min) * P + j;
        return k < 0 ? 0 : (k > n - 2 ? n - 2 : k);
    }
    inline double at(double p, double* dfdp) const noexcept {
        const int k = segment(p);
        const double s = (p - pos[k]) / (pos[k + 1] - pos[k]);
        double v, dv;
        eval(k, s, v, dv);
        *dfdp = dv;
        return v;
    }
};

/// Uniform nodes on [x_min, x_max].
struct Uni : Hermite {
    double x0 = 0, h = 1, inv_h = 1;

    void setup(int n_, double x_min, double x_max) {
        n = n_;
        x0 = x_min;
        h = (x_max - x_min) / (n - 1);
        inv_h = 1.0 / h;
        pos.resize(n);
        f.resize(n);
        d1.resize(n);
        d2.resize(n);
        for (int k = 0; k < n; ++k)
            pos[k] = x_min + k * h;
    }
    inline double at(double x, double* dfdx) const noexcept {
        int k = static_cast<int>((x - x0) * inv_h);
        if (k > n - 2)
            k = n - 2;
        if (k < 0)
            k = 0;
        const double s = (x - pos[k]) * inv_h;
        double v, dv;
        eval(k, s, v, dv);
        *dfdx = dv;
        return v;
    }
};

/// 20-point Gauss-Legendre on [a, b].
template <typename F> double gl20(double a, double b, F&& fn) {
    static const double x[10] = {0.0765265211334973337546404, 0.2277858511416450780804962,
                                 0.3737060887154195606725482, 0.5108670019508270980043641,
                                 0.6360536807265150254528367, 0.7463319064601507926143051,
                                 0.8391169718222188233945291, 0.9122344282513259058677524,
                                 0.9639719272779137912676661, 0.9931285991850949247861224};
    static const double w[10] = {0.1527533871307258506980843, 0.1491729864726037467878287,
                                 0.1420961093183820513292983, 0.1316886384491766268984945,
                                 0.1181945319615184173123774, 0.1019301198172404350367501,
                                 0.0832767415767047487247581, 0.0626720483341090635695065,
                                 0.0406014298003869413310400, 0.0176140071391521183118620};
    const double c = 0.5 * (a + b), hh = 0.5 * (b - a);
    double s = 0;
    for (int i = 0; i < 10; ++i)
        s += w[i] * (fn(c - hh * x[i]) + fn(c + hh * x[i]));
    return s * hh;
}

struct Tables {
    Geo Fs, Fw, H, Gs, Gc, Phis, Psi, Gams, Gamz;
    Uni Phiw;
    double Ginf = 0, tauF_max = 0, tauH_max = 0, C_F = 0, C_H = 0;

    Tables() {
        const int P = ExactPondedDepth::kNodesPerOctave;
        const double m = kM;
        auto pw = [](double a, double b) { return std::pow(a, b); };
        auto gFu = [&](double u) { return 1.0 / (1.0 - pw(u, m)); };
        auto gFw = [&](double w) { return 1.0 / (1.0 - pw(1.0 - w, m)); };
        auto gG = [&](double u) { return 1.0 / (1.0 + pw(u, m)); };
        auto gH = [&](double v) { return 1.0 / (pw(1.0 + v, m) - 1.0); };

        // ---- forward tables
        Fs.setup(P, -60, -1);
        Fw.setup(P, -40, -1);
        H.setup(P, -40, 60);
        Gs.setup(P, -60, 0);
        Gc.setup(P, 0, 60);
        std::vector<double> accFs(Fs.n), accFw(Fw.n), accH(H.n), accGs(Gs.n), accGc(Gc.n);
        accFs[0] = Fs.pos[0]; // F(u) = u + u^(m+1)/(m+1) + ..., the second term ~1e-48
        for (int k = 1; k < Fs.n; ++k)
            accFs[k] = accFs[k - 1] + gl20(Fs.pos[k - 1], Fs.pos[k], gFu);
        accFw[Fw.n - 1] = accFs[Fs.n - 1]; // w = 1/2 is u = 1/2
        for (int k = Fw.n - 2; k >= 0; --k)
            accFw[k] = accFw[k + 1] + gl20(Fw.pos[k], Fw.pos[k + 1], gFw);
        {
            const double U = 1.0 + H.pos[H.n - 1];
            double tail = 0; // sum_j U^(1-m(j+1))/(m(j+1)-1)
            for (int j = 0; j < 6; ++j)
                tail += pw(U, 1 - m * (j + 1)) / (m * (j + 1) - 1);
            accH[H.n - 1] = tail;
            for (int k = H.n - 2; k >= 0; --k)
                accH[k] = accH[k + 1] + gl20(H.pos[k], H.pos[k + 1], gH);
        }
        accGs[0] = Gs.pos[0];
        for (int k = 1; k < Gs.n; ++k)
            accGs[k] = accGs[k - 1] + gl20(Gs.pos[k - 1], Gs.pos[k], gG);
        {
            const double U = Gc.pos[Gc.n - 1];
            double tail = 0; // sum_j (-1)^j U^(1-m(j+1))/(m(j+1)-1)
            for (int j = 0; j < 6; ++j)
                tail += ((j & 1) ? -1.0 : 1.0) * pw(U, 1 - m * (j + 1)) / (m * (j + 1) - 1);
            accGc[Gc.n - 1] = tail;
            for (int k = Gc.n - 2; k >= 0; --k)
                accGc[k] = accGc[k + 1] + gl20(Gc.pos[k], Gc.pos[k + 1], gG);
        }
        Ginf = accGs[Gs.n - 1] + accGc[0];
        for (int k = 0; k < Fs.n; ++k) {
            const double u = Fs.pos[k], um = pw(u, m);
            Fs.f[k] = accFs[k];
            Fs.d1[k] = 1.0 / (1.0 - um);
            Fs.d2[k] = m * pw(u, m - 1) / ((1.0 - um) * (1.0 - um));
        }
        for (int k = 0; k < Fw.n; ++k) {
            const double w = Fw.pos[k], u = 1.0 - w, um = pw(u, m);
            Fw.f[k] = accFw[k];
            Fw.d1[k] = -1.0 / (1.0 - um);
            Fw.d2[k] = m * pw(u, m - 1) / ((1.0 - um) * (1.0 - um));
        }
        for (int k = 0; k < H.n; ++k) {
            const double u = 1.0 + H.pos[k], um = pw(u, m);
            H.f[k] = accH[k];
            H.d1[k] = -1.0 / (um - 1.0);
            H.d2[k] = m * pw(u, m - 1) / ((um - 1.0) * (um - 1.0));
        }
        for (int k = 0; k < Gs.n; ++k) {
            const double u = Gs.pos[k], um = pw(u, m);
            Gs.f[k] = accGs[k];
            Gs.d1[k] = 1.0 / (1.0 + um);
            Gs.d2[k] = -m * pw(u, m - 1) / ((1.0 + um) * (1.0 + um));
        }
        for (int k = 0; k < Gc.n; ++k) {
            const double u = Gc.pos[k], um = pw(u, m);
            Gc.f[k] = accGc[k];
            Gc.d1[k] = -1.0 / (1.0 + um);
            Gc.d2[k] = m * pw(u, m - 1) / ((1.0 + um) * (1.0 + um));
        }
        {
            const double w0 = Fw.pos[0], v0 = H.pos[0];
            C_F = accFw[0] + std::log(w0) / m + (m - 1) / (2 * m) * w0;
            C_H = accH[0] + std::log(v0) / m - (m - 1) / (2 * m) * v0;
            tauF_max = C_F + 53.0 * std::log(2.0) / m;
            tauH_max = C_H + 53.0 * std::log(2.0) / m;
        }

        // Evaluate a forward function anywhere: the node above plus one Gauss-Legendre.
        auto at_ld = [&](const Geo& T, const std::vector<double>& acc, double p, auto&& g,
                         bool increasing) {
            int k =
                static_cast<int>(std::lower_bound(T.pos.begin(), T.pos.end(), p) - T.pos.begin());
            if (k >= T.n)
                k = T.n - 1;
            const double seg = gl20(p, T.pos[k], g);
            return increasing ? acc[k] - seg : acc[k] + seg;
        };
        // Solve f(p) = target on a forward table by Newton from the bracketing node.
        auto invert = [&](const Geo& T, const std::vector<double>& acc, double target, auto&& g,
                          bool increasing, auto&& dfdp) {
            int lo = 0, hi = T.n - 1;
            while (hi - lo > 1) {
                const int mid = (lo + hi) / 2;
                const bool left = increasing ? (acc[mid] <= target) : (acc[mid] >= target);
                if (left)
                    lo = mid;
                else
                    hi = mid;
            }
            double p = T.pos[lo];
            for (int it = 0; it < 80; ++it) {
                const double step = (at_ld(T, acc, p, g, increasing) - target) / dfdp(p);
                p -= step;
                if (p < T.pos[0])
                    p = T.pos[0];
                if (p > T.pos[T.n - 1])
                    p = T.pos[T.n - 1];
                if (std::fabs(step) <= 1e-17 * p)
                    break;
            }
            return p;
        };
        auto dFs = [&](double u) { return 1.0 / (1.0 - pw(u, m)); };
        auto dFw = [&](double w) { return -1.0 / (1.0 - pw(1.0 - w, m)); };
        auto dH = [&](double v) { return -1.0 / (pw(1.0 + v, m) - 1.0); };
        auto dGs = [&](double u) { return 1.0 / (1.0 + pw(u, m)); };
        auto dGc = [&](double u) { return -1.0 / (1.0 + pw(u, m)); };

        // ---- Phis: u(tau), geometric tau in [2^-60, 1/2]
        Phis.setup(P, -60, -1);
        for (int k = 0; k < Phis.n; ++k) {
            const double tau = Phis.pos[k];
            double u = (tau <= accFs[0]) ? tau : invert(Fs, accFs, tau, gFu, true, dFs);
            if (tau > accFs[Fs.n - 1]) // just above F(1/2): the overlap with the w grid
                u = 1.0 - invert(Fw, accFw, tau, gFw, false, dFw);
            const double um = pw(u, m), up = 1.0 - um;
            Phis.f[k] = u;
            Phis.d1[k] = up;
            Phis.d2[k] = -m * pw(u, m - 1) * up;
        }
        // ---- Phiw: w(tau), uniform tau in [1/4, tauF_max]
        Phiw.setup(1025, 0.25, tauF_max);
        for (int k = 0; k < Phiw.n; ++k) {
            const double tau = Phiw.pos[k];
            double w;
            if (tau >= accFw[0])
                w = std::exp(-m * (tau - C_F));
            else if (tau <= accFs[Fs.n - 1])
                w = 1.0 - invert(Fs, accFs, tau, gFu, true, dFs);
            else
                w = invert(Fw, accFw, tau, gFw, false, dFw);
            const double u = 1.0 - w, um = pw(u, m), up = 1.0 - um;
            Phiw.f[k] = w;
            Phiw.d1[k] = -up;
            Phiw.d2[k] = m * pw(u, m - 1) * up;
        }
        // ---- Psi: v(tau), geometric tau in [2^-44, 2^5]
        Psi.setup(P, -44, 5);
        for (int k = 0; k < Psi.n; ++k) {
            const double tau = Psi.pos[k];
            double v;
            if (tau >= accH[0])
                v = std::exp(-m * (tau - C_H));
            else if (tau <= accH[H.n - 1])
                v = pw(tau * (m - 1), -1.0 / (m - 1)) - 1.0;
            else
                v = invert(H, accH, tau, gH, false, dH);
            const double u = 1.0 + v, um = pw(u, m);
            Psi.f[k] = v;
            Psi.d1[k] = -(um - 1.0);
            Psi.d2[k] = m * pw(u, m - 1) * (um - 1.0);
        }
        // ---- Gams: u(y), geometric y in [2^-60, 1]
        Gams.setup(P, -60, 0);
        for (int k = 0; k < Gams.n; ++k) {
            const double y = Gams.pos[k];
            double u;
            if (y <= accGs[0])
                u = y;
            else if (y <= accGs[Gs.n - 1])
                u = invert(Gs, accGs, y, gG, true, dGs);
            else
                u = invert(Gc, accGc, Ginf - y, gG, false, dGc);
            const double um = pw(u, m), dudy = 1.0 + um;
            Gams.f[k] = u;
            Gams.d1[k] = dudy;
            Gams.d2[k] = m * pw(u, m - 1) * dudy;
        }
        // ---- Gamz: u(z), geometric z in [2^-60, 1]
        Gamz.setup(P, -60, 0);
        for (int k = 0; k < Gamz.n; ++k) {
            const double z = Gamz.pos[k];
            double u;
            if (z <= accGc[Gc.n - 1])
                u = pw(z * (m - 1), -1.0 / (m - 1));
            else
                u = invert(Gc, accGc, z, gG, false, dGc);
            const double um = pw(u, m);
            Gamz.f[k] = u;
            Gamz.d1[k] = -(1.0 + um);
            Gamz.d2[k] = m * pw(u, m - 1) * (1.0 + um);
        }
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

/// u^m for m = 5/3 without pow.
inline double upow(double u) noexcept {
    const double c = std::cbrt(u);
    return u * c * c;
}

} // namespace

double ExactPondedDepth::alphaScale(double alpha) noexcept {
    return alpha > 0.0 ? std::pow(alpha, -1.0 / kExponent) : 0.0;
}

void ExactPondedDepth::prepare() {
    (void)tables();
}

double ExactPondedDepth::step(double depth, double inflow, double alpha, double alpha_scale,
                              double dStore, double dt, double& t_runoff, double& outflow,
                              ScaleCache& cache) noexcept {
    const Tables& T = tables();
    constexpr double m = kExponent, mm1 = kExponent - 1.0, inv_m = 1.0 / kExponent;
    double tx = dt;
    outflow = 0.0;
    if (depth + inflow * tx <= dStore) {
        depth += inflow * tx;
        t_runoff = tx;
        return std::max(depth, 0.0);
    }
    const double dx = dStore - depth;
    if (dx > 0.0 && inflow > 0.0) {
        tx -= dx / inflow;
        depth = dStore;
    }
    if (!(alpha > 0.0 && tx > 0.0)) {
        tx = std::max(tx, 0.0);
        depth += inflow * tx;
        t_runoff = tx;
        return std::max(depth, 0.0);
    }
    const double x0 = std::max(depth - dStore, 0.0);
    double x1;
    if (inflow == 0.0) {
        // x1 = x0 y^(-3/2), y = 1 + (2/3) alpha x0^(2/3) tx; outflow = alpha x1 x0^(2/3) / y
        if (x0 <= 0.0) {
            x1 = 0.0;
        } else {
            const double c = std::cbrt(x0);
            const double x0m1 = c * c;
            const double y = 1.0 + mm1 * alpha * x0m1 * tx;
            x1 = x0 / (y * std::sqrt(y));
            outflow = alpha * x1 * x0m1 / y;
        }
    } else {
        const double a = std::fabs(inflow);
        double a_pow;
        if (cache.key == a) {
            a_pow = cache.value;
        } else {
            a_pow = std::pow(a, inv_m);
            cache.key = a;
            cache.value = a_pow;
        }
        const double xe = a_pow * alpha_scale;
        const double u0 = x0 / xe;
        const double dtau = tx * a / xe;
        double slope;
        if (inflow > 0.0) {
            if (u0 < 1.0) {
                double tau0;
                if (u0 <= 0.0)
                    tau0 = 0.0;
                else if (u0 <= 0.5)
                    tau0 = (u0 < T.Fs.pos[0]) ? u0 : T.Fs.at(u0, &slope);
                else {
                    const double w0 = 1.0 - u0;
                    tau0 = (w0 < T.Fw.pos[0]) ? T.C_F - inv_m * std::log(w0) : T.Fw.at(w0, &slope);
                }
                const double tau1 = tau0 + dtau;
                if (tau1 >= T.tauF_max) {
                    x1 = xe;
                    outflow = a;
                } else if (tau1 < 0.5) {
                    const double u1 = (tau1 < T.Phis.pos[0]) ? tau1 : T.Phis.at(tau1, &slope);
                    x1 = u1 * xe;
                    outflow = a * upow(u1);
                } else {
                    const double w1 = T.Phiw.at(tau1, &slope); // dw/dtau = -(1 - u^m)
                    x1 = (1.0 - w1) * xe;
                    outflow = a * (1.0 + slope);
                }
            } else if (u0 == 1.0) {
                x1 = x0;
                outflow = a;
            } else {
                const double v0 = u0 - 1.0;
                double tau0;
                if (v0 > T.H.pos[T.H.n - 1])
                    tau0 = std::pow(u0, 1.0 - m) / mm1;
                else if (v0 < T.H.pos[0])
                    tau0 = T.C_H - inv_m * std::log(v0);
                else
                    tau0 = T.H.at(v0, &slope);
                const double tau1 = tau0 + dtau;
                if (tau1 >= T.tauH_max) {
                    x1 = xe;
                    outflow = a;
                } else if (tau1 < T.Psi.pos[0]) {
                    const double u1 = std::pow(tau1 * mm1, -1.0 / mm1);
                    x1 = u1 * xe;
                    outflow = a * upow(u1);
                } else {
                    const double v1 = T.Psi.at(tau1, &slope); // dv/dtau = -(u^m - 1)
                    x1 = (1.0 + v1) * xe;
                    outflow = a * (1.0 - slope);
                }
            }
        } else {
            // Net loss. y = time left until the excess drains, z = Ginf - y.
            double u1;
            if (u0 <= 1.0) {
                const double y0 = (u0 <= 0.0) ? 0.0 : (u0 < T.Gs.pos[0]) ? u0 : T.Gs.at(u0, &slope);
                if (dtau >= y0) {
                    const double t_hit = y0 * xe / a;
                    depth = dStore - a * (tx - t_hit);
                    t_runoff = tx;
                    return std::max(depth, 0.0);
                }
                const double y1 = y0 - dtau;
                if (y1 < T.Gams.pos[0]) {
                    u1 = y1;
                    outflow = a * upow(u1);
                } else {
                    u1 = T.Gams.at(y1, &slope); // du/dy = 1 + u^m
                    outflow = (u1 < 0.5) ? a * upow(u1) : a * (slope - 1.0);
                }
            } else {
                const double z0 =
                    (u0 > T.Gc.pos[T.Gc.n - 1]) ? std::pow(u0, 1.0 - m) / mm1 : T.Gc.at(u0, &slope);
                const double z1 = z0 + dtau;
                if (z1 >= T.Ginf) {
                    const double t_hit = (T.Ginf - z0) * xe / a;
                    depth = dStore - a * (tx - t_hit);
                    t_runoff = tx;
                    return std::max(depth, 0.0);
                }
                if (z1 < T.Gamz.pos[0]) {
                    u1 = std::pow(z1 * mm1, -1.0 / mm1);
                    outflow = a * upow(u1);
                } else if (z1 <= 1.0) {
                    u1 = T.Gamz.at(z1, &slope); // du/dz = -(1 + u^m)
                    outflow = a * (-slope - 1.0);
                } else {
                    const double y1 = T.Ginf - z1;
                    if (y1 < T.Gams.pos[0]) {
                        u1 = y1;
                        outflow = a * upow(u1);
                    } else {
                        u1 = T.Gams.at(y1, &slope);
                        outflow = (u1 < 0.5) ? a * upow(u1) : a * (slope - 1.0);
                    }
                }
            }
            x1 = u1 * xe;
        }
    }
    depth = dStore + x1;
    t_runoff = tx;
    return std::max(depth, 0.0);
}

} // namespace runoff
} // namespace openswmm
