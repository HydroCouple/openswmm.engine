#!/usr/bin/env python3
"""
Prototype: the 2D ROM's deviation operator under MOMENTUM_EQUATION FULL_SWE.

Mirrors local_inertial_decay.py, but for the FULL_SWE face law as implemented
in src/engine/2d/solver/SweKernels.hpp + ExplicitInertialSolver.cpp
(RECONSTRUCTION_ORDER 1, the default):
  * cell state (h, q = h u); first-order Godunov, forward Euler, CFL <= 0.5
  * hydrostatic reconstruction (Audusse 2004) at the face bed z_f = max(zL, zR):
        h*_side = max(0, eta_side - z_f)
  * HLLC flux (1D: HLL) with the two-rarefaction wave speeds of waveSpeeds()
  * per-side bed-pressure correction  1/2 g (h*^2 - h^2)
  * semi-implicit Manning friction per CELL after the flux update, against the
    NEW depth and |q| of the flux-updated discharge (frictionUpdate()).

Part A  (operator form) periodic flat bed + constant body force g h S_drive that
        sustains a uniform Manning flow; seed a cosine depth deviation on the
        slow manifold and measure envelope decay (-> D) and phase speed (-> c_k).
        Prediction: advection-diffusion with
            D   = D_phys + D_num,   D_phys = h^(10/3)/(2 n^2 q0)  (Manning)
                                    D_num  = dx (c^2 - u^2) / (2 c)   (HLL mass
                                             dissipation, c = sqrt(g h))
            c_k = (5/3) u0.
        D_num is mesh-dependent and independent of n.

Part B  (steady sensitivity on a REAL sloped bed) 1D chute with a mass source in
        the top cell and a transmissive outlet; run to steady for n and 1.1 n
        and report d ln h / d ln n in the uniform reach. Manning: 3/5. Compared
        against the local-inertial face law (InertialKernels.hpp) on the same
        bed, as a function of the bed step per cell dz = S dx relative to h.
"""
import numpy as np

G = 9.80665


# ─── FULL_SWE kernels (1D normal-frame transcription of SweKernels.hpp) ─────
def wave_speeds(hl, ul, hr, ur):
    cl, cr = np.sqrt(G * hl), np.sqrt(G * hr)
    ustar = 0.5 * (ul + ur) + cl - cr
    cstar = 0.5 * (cl + cr) + 0.25 * (ul - ur)
    sl = np.minimum(ul - cl, ustar - cstar)
    sr = np.maximum(ur + cr, ustar + cstar)
    dry_r = (hl > 0) & (hr <= 0)
    dry_l = (hl <= 0) & (hr > 0)
    sl = np.where(dry_r, ul - cl, np.where(dry_l, ur - 2 * cr, sl))
    sr = np.where(dry_r, ul + 2 * cl, np.where(dry_l, ur + cr, sr))
    both = (hl <= 0) & (hr <= 0)
    return np.where(both, 0.0, sl), np.where(both, 0.0, sr)


def hll(hl, ul, hr, ur):
    sl, sr = wave_speeds(hl, ul, hr, ur)
    fhl, fnl = hl * ul, hl * ul * ul + 0.5 * G * hl * hl
    fhr, fnr = hr * ur, hr * ur * ur + 0.5 * G * hr * hr
    den = np.where(sr - sl > 1e-14, sr - sl, 1.0)
    fh = (sr * fhl - sl * fhr + sl * sr * (hr - hl)) / den
    fn = (sr * fnl - sl * fnr + sl * sr * (hr * ur - hl * ul)) / den
    fh = np.where(sl >= 0, fhl, np.where(sr <= 0, fhr, fh))
    fn = np.where(sl >= 0, fnl, np.where(sr <= 0, fnr, fn))
    both = (hl <= 0) & (hr <= 0)
    return np.where(both, 0, fh), np.where(both, 0, fn)


def swe_face(etaL, hL, qL, etaR, hR, qR, hdry):
    zL, zR = etaL - hL, etaR - hR
    zf = np.maximum(zL, zR)
    hl = np.maximum(etaL - zf, 0.0)
    hr = np.maximum(etaR - zf, 0.0)
    ul = np.where((hL > hdry) & (hl > 0), qL / np.maximum(hL, 1e-300), 0.0)
    ur = np.where((hR > hdry) & (hr > 0), qR / np.maximum(hR, 1e-300), 0.0)
    fh, fn = hll(hl, ul, hr, ur)
    cL = 0.5 * G * (hl * hl - hL * hL)        # L gets  -F + cL
    cR = -0.5 * G * (hr * hr - hR * hR)       # R gets  +F + cR
    return fh, fn, cL, cR


def friction(q, h, n, dt):
    h73 = h * h * np.cbrt(h)
    return np.where(h > 0, q / (1.0 + G * dt * n * n * np.abs(q) / np.maximum(h73, 1e-300)), 0.0)


# ─── Part A: operator form on a periodic flat bed ───────────────────────────
def run_A(h0, u0, n, Lx, N, m=1, amp=1e-4):
    dx = Lx / N
    x = (np.arange(N) + 0.5) * dx
    q0 = u0 * h0
    k = 2 * np.pi * m / Lx
    c = np.sqrt(G * h0)
    r_f = G * n * n * q0 / h0 ** (7 / 3)
    S_drive = r_f * q0 / (G * h0)
    D_phys = h0 ** (10 / 3) / (2 * n * n * q0)
    D_num = dx * (c * c - u0 * u0) / (2 * c)
    ck_pred = 5 / 3 * u0
    ip, im = (np.arange(N) + 1) % N, (np.arange(N) - 1) % N
    h = h0 + amp * h0 * np.cos(k * x)
    # slow-manifold seed: kinematic q = q0 (h/h0)^(5/3) minus the diffusive part
    q = q0 * (h / h0) ** (5 / 3) + D_phys * amp * h0 * k * np.sin(k * x)
    dt = 0.5 * dx / (u0 + c)
    gamma_pred = (D_phys + D_num) * k * k
    t_end = max(5.0 / gamma_pred, 15.0 / r_f)
    ns = min(300000, int(t_end / dt) + 1)
    ac, as_, tt = [], [], []
    cs, sn = np.cos(k * x), np.sin(k * x)
    for s in range(ns):
        fh, fn, cL, cR = swe_face(h, h, q, h[ip], h[ip], q[ip], 1e-3)
        hn = h - dt / dx * (fh - fh[im])
        qn = q + dt / dx * (-(fn - cL) + (fn[im] + cR[im])) + dt * G * hn * S_drive
        q = friction(qn, hn, n, dt)
        h = hn
        d = h - h.mean()
        ac.append(2 / N * d @ cs); as_.append(2 / N * d @ sn); tt.append((s + 1) * dt)
    ac, as_, tt = map(np.array, (ac, as_, tt))
    env = np.hypot(ac, as_)
    ph = np.unwrap(np.arctan2(as_, ac))
    lo = int(np.searchsorted(tt, 10.0 / r_f))
    lo = min(max(lo, 1), ns - 30)
    hi = ns - 1
    A = np.vstack([tt[lo:hi], np.ones(hi - lo)]).T
    g = -np.linalg.lstsq(A, np.log(env[lo:hi]), rcond=None)[0][0]
    ck = abs(np.linalg.lstsq(A, ph[lo:hi], rcond=None)[0][0]) / k
    return dict(Lam=r_f / (c * k), D_phys=D_phys, D_num=D_num, D_meas=g / k / k,
                ck_pred=ck_pred, ck_meas=ck, Fr=u0 / c)


# ─── Part B: steady Manning sensitivity on a sloped bed ─────────────────────
def li_face_q(q, hf, dt, Sf, n):
    h73 = hf * hf * np.cbrt(hf)
    num = q - G * hf * dt * Sf
    den = 1.0 + G * dt * n * n * np.abs(q) / np.maximum(h73, 1e-300)
    qs = num / den
    cap = 1.5 * hf * np.sqrt(G * hf)
    return np.where(hf > 1e-6, np.clip(qs, -cap, cap), 0.0)


def chute(scheme, n, S, dx, q_in, N=120, t_end=None):
    x = (np.arange(N) + 0.5) * dx
    z = S * (N * dx - x)
    hN = (n * q_in / np.sqrt(S)) ** 0.6
    h = np.full(N, hN)
    q = np.full(N, q_in)        # FULL_SWE cell discharge
    qf = np.full(N - 1, q_in)   # LI face discharge (face i between i, i+1)
    c = np.sqrt(G * hN)
    u = q_in / hN
    dt = (0.5 if scheme == "swe" else 0.7) * dx / (u + c)
    if t_end is None:
        t_end = 40 * N * dx / (5 / 3 * u)
    for _ in range(int(t_end / dt)):
        eta = z + h
        if scheme == "swe":
            fh, fn, cL, cR = swe_face(eta[:-1], h[:-1], q[:-1], eta[1:], h[1:], q[1:], 1e-4)
            # interior faces; outlet: transmissive (ghost = last cell), inlet: wall
            fo_h = q[-1]; fo_n = q[-1] ** 2 / max(h[-1], 1e-12) + 0.5 * G * h[-1] ** 2
            Fh = np.concatenate(([0.0], fh, [fo_h]))
            Fn = np.concatenate(([0.5 * G * h[0] ** 2], fn, [fo_n]))
            CL = np.concatenate((cL, [0.0]))   # correction for left cell of face j=1..N
            CR = np.concatenate(([0.0], cR))   # correction for right cell of face j=0..N-1
            hn = h - dt / dx * (Fh[1:] - Fh[:-1])
            hn[0] += dt * q_in / dx
            qn = q + dt / dx * (-(Fn[1:] - CL) + (Fn[:-1] + CR))
            hn = np.maximum(hn, 0.0)
            q = friction(qn, hn, n, dt)
            h = hn
        else:
            hf = np.maximum(eta[:-1], eta[1:]) - np.maximum(z[:-1], z[1:])
            Sf = (eta[1:] - eta[:-1]) / dx
            qf = li_face_q(qf, hf, dt, Sf, n)
            qo = h[-1] ** (5 / 3) * np.sqrt(S) / n     # normal-flow outlet
            F = np.concatenate(([0.0], qf, [qo]))
            h = h - dt / dx * (F[1:] - F[:-1])
            h[0] += dt * q_in / dx
            h = np.maximum(h, 0.0)
    sl = slice(N // 3, 2 * N // 3)
    return h[sl].mean(), hN


def elasticity(scheme, S, dx, q_in):
    h1, hN = chute(scheme, 0.03, S, dx, q_in)
    h2, _ = chute(scheme, 0.033, S, dx, q_in)
    return np.log(h2 / h1) / np.log(1.1), h1, hN


def main():
    print("Part A: FULL_SWE deviation operator on a sustained uniform Manning flow")
    print("  (flat periodic bed + body force; Lx = 4000 m, N = 800, dx = 5 m, mode 1)")
    print("{:>6} {:>6} {:>6} {:>7} {:>9} {:>9} {:>9} {:>11} {:>8} {:>8}".format(
        "h", "n", "Fr", "Lam", "D_phys", "D_num", "D_meas", "meas/(p+n)", "ck/pred", "D_num/Dp"))
    for h0, u0, n in [(0.40, 0.84, 0.03), (0.40, 0.84, 0.06), (0.10, 0.33, 0.03),
                      (0.03, 0.146, 0.03), (0.01, 0.07, 0.03), (1.0, 1.0, 0.04)]:
        r = run_A(h0, u0, n, 4000.0, 800)
        print("{:6.3f} {:6.3f} {:6.3f} {:7.1f} {:9.3g} {:9.3g} {:9.3g} {:11.3f} {:8.3f} {:8.2f}".format(
            h0, n, r["Fr"], r["Lam"], r["D_phys"], r["D_num"], r["D_meas"],
            r["D_meas"] / (r["D_phys"] + r["D_num"]), r["ck_meas"] / r["ck_pred"],
            r["D_num"] / r["D_phys"]))
    print()
    print("Part B: steady d ln h / d ln n on a sloped chute (Manning = 0.600)")
    print("{:>7} {:>5} {:>8} {:>8} {:>7} {:>9} {:>9}".format(
        "S", "dx", "q_in", "h_norm", "dz/h", "e(LI)", "e(SWE)"))
    for S, dx, q in [(0.002, 5, 0.04), (0.002, 5, 0.004), (0.02, 5, 0.04), (0.02, 5, 0.004),
                     (0.02, 5, 0.0004), (0.02, 1, 0.004), (0.02, 20, 0.004), (0.002, 5, 0.0004)]:
        eli, _, hN = elasticity("li", S, dx, q)
        esw, _, _ = elasticity("swe", S, dx, q)
        print("{:7.3f} {:5.0f} {:8.4f} {:8.4f} {:7.2f} {:9.3f} {:9.3f}".format(
            S, dx, q, hN, S * dx / hN, eli, esw))


if __name__ == "__main__":
    main()
