#!/usr/bin/env python3
"""
2D extension of local_inertial_decay.py: does the deviation operator's clean
collapse (and the ~0.71 diffusion factor) survive on a structured 2D mesh with
the vector-magnitude friction, and is the diffusion ISOTROPIC?

Why 2D can differ from 1D
-------------------------
The real kernel frictions the normal discharge with the FLOW-VECTOR magnitude
|q_vec| (Perot reconstruction), not |q_n| (InertialKernels.hpp is explicit about
this — it is what prevents the 45-degree checkerboarding). Linearizing about a
uniform base flow u0 in +x (|q_vec| = q0):

    streamwise x-face:  d(qx|q_vec|)/dqx = |q_vec| + qx^2/|q_vec| = 2 q0  -> rate 2 r_f
    transverse y-face:  d(qy|q_vec|)/dqy = |q_vec| + qy^2/|q_vec| = q0   -> rate  r_f
        (the cross terms d/dq_other vanish at the base: qy = 0)

Slaved diffusion coefficient  D = g h / r_lin :
    D_x = g h / (2 r_f) = K_eff/2          (streamwise; matches the 1D result)
    D_y = g h /  r_f    = K_eff            (transverse)  => D_y / D_x = 2

So the operator is predicted ANISOTROPIC, aligned with the flow. Plus advection
c_k = (5/3) u0 in the flow direction only. The 1D prototype measured a further
scheme factor ~0.71 on D_x; this script checks whether that factor holds for D_x
AND D_y on a 2D grid, and whether D_y/D_x ~ 2.

Operator claim:  d(dh)/dt = D_x d2/dx2 + D_y d2/dy2 (dh) - c_k d/dx (dh)
Method: periodic structured grid, sustained uniform Manning flow in +x, seed a
plane-wave depth deviation, measure envelope decay (diffusion) and phase speed
(advection) by complex projection, for streamwise / transverse / diagonal modes.
"""
import numpy as np

G = 9.80665


def face_update(q, hf, dt, Sf, n2, qmag, fr_max=1.5):
    """Exact inertialFaceUpdate + froudeCap, vectorized over a face array."""
    h73 = hf * hf * np.cbrt(hf)
    qstar = (q - G * hf * dt * Sf) / (1.0 + G * dt * n2 * qmag / h73)
    qcap = fr_max * hf * np.sqrt(G * hf)
    return np.clip(qstar, -qcap, qcap)


def run_case(h0, u0, n, L, N, mx, my, seed_amp=1e-4):
    """Periodic NxN grid; plane wave (mx,my). Returns measured D along the mode
    and the advection speed. Base uniform flow u0 in +x, sustained by S_drive."""
    dx = L / N
    q0 = u0 * h0
    c = np.sqrt(G * h0)
    r_f = G * n * n * q0 / h0 ** (7.0 / 3.0)
    K_eff = h0 ** (10.0 / 3.0) / (n * n * q0)          # = g h / r_f
    S_drive = r_f * q0 / (G * h0)                       # sustains base x-flow
    kx = 2.0 * np.pi * mx / L
    ky = 2.0 * np.pi * my / L
    k2 = kx * kx + ky * ky

    xs = (np.arange(N) + 0.5) * dx
    X, Y = np.meshgrid(xs, xs, indexing="ij")
    phase0 = kx * X + ky * Y
    dh = seed_amp * h0 * np.cos(phase0)
    h = h0 + dh
    # seed fluxes near the (approx) slow manifold: dq = -D0 grad(dh), D0 ~ K/2
    D0 = 0.5 * K_eff
    ddx = (np.roll(dh, -1, 0) - dh) / dx                # d(dh)/dx at x-face
    ddy = (np.roll(dh, -1, 1) - dh) / dx                # d(dh)/dy at y-face
    qx = q0 - D0 * ddx
    qy = 0.0 - D0 * ddy

    dt = 0.12 * dx / c
    gamma_pred = D0 * k2                                # rough, for window sizing
    t_end = max(6.0 / max(gamma_pred, 1e-9), 12.0 / r_f)
    nsteps = min(300000, max(400, int(t_end / dt)))

    cbase, sbase = np.cos(phase0), np.sin(phase0)
    ac, as_, times = [], [], []
    n2 = n * n
    for s in range(nsteps):
        eta = h                                         # flat bed
        # Perot cell-centered discharge vector -> |q_vec| per cell
        ux = 0.5 * (qx + np.roll(qx, 1, 0))             # east + west x-faces
        uy = 0.5 * (qy + np.roll(qy, 1, 1))             # north + south y-faces
        spd = np.sqrt(ux * ux + uy * uy)
        # face friction magnitudes = average of the two adjacent cell speeds
        qmag_x = 0.5 * (spd + np.roll(spd, -1, 0))
        qmag_y = 0.5 * (spd + np.roll(spd, -1, 1))
        # x-faces
        hfx = np.maximum(np.maximum(eta, np.roll(eta, -1, 0)), 1e-9)
        Sfx = (np.roll(eta, -1, 0) - eta) / dx - S_drive
        qx = face_update(qx, hfx, dt, Sfx, n2, qmag_x)
        # y-faces
        hfy = np.maximum(np.maximum(eta, np.roll(eta, -1, 1)), 1e-9)
        Sfy = (np.roll(eta, -1, 1) - eta) / dx
        qy = face_update(qy, hfy, dt, Sfy, n2, qmag_y)
        # continuity: divergence of face discharges
        div = (qx - np.roll(qx, 1, 0)) + (qy - np.roll(qy, 1, 1))
        h = h - (dt / dx) * div

        dev = h - h.mean()
        ac.append((2.0 / (N * N)) * np.sum(dev * cbase))
        as_.append((2.0 / (N * N)) * np.sum(dev * sbase))
        times.append((s + 1) * dt)

    ac, as_, times = np.array(ac), np.array(as_), np.array(times)
    env = np.hypot(ac, as_)
    phase = np.unwrap(np.arctan2(as_, ac))

    t_fast = 5.0 / r_f
    lo = min(max(int(np.searchsorted(times, t_fast)), 1), nsteps - 20)
    target = env[lo] * np.exp(-3.0)
    idx = np.where(env[lo:] < target)[0]
    hi = (lo + idx[0]) if len(idx) else nsteps - 1
    if hi - lo < 20:
        hi = min(lo + max(20, nsteps // 3), nsteps - 1)
    A = np.vstack([times[lo:hi], np.ones(hi - lo)]).T
    gamma = -np.linalg.lstsq(A, np.log(env[lo:hi] + 1e-300), rcond=None)[0][0]
    dphi = np.linalg.lstsq(A, phase[lo:hi], rcond=None)[0][0]
    # advection speed along flow: phase advances at c_k * kx
    ck_meas = abs(dphi) / kx if kx > 0 else 0.0

    D_meas = gamma / k2
    eps = G * h0 * k2 / (r_f * r_f)      # inertial inflation parameter O(Fr^2)
    return dict(K_eff=K_eff, D_meas=D_meas, gamma=gamma, ck_meas=ck_meas,
                kx=kx, ky=ky, r_f=r_f, eps=eps)


def main():
    L, N, n, u0, h0 = 2000.0, 96, 0.12, 1.0, 1.0
    Ke = h0 ** (10.0 / 3.0) / (n * n * u0 * h0)
    print("2D local-inertial deviation operator: anisotropy + scheme factor check")
    print("=" * 100)
    print(f"Grid {N}x{N}, L={L:.0f} m (dx={L/N:.2f}), h0={h0}, u0={u0} (+x, Fr=0.32), n={n}.")
    print("Predicted:  D_x = f*(K_eff/2)  streamwise,  D_y = f*(K_eff)  transverse  (D_y/D_x=2),")
    print("            c_k = (5/3)u0 = 1.667 in the FLOW (x) direction only.")
    print(f"K_eff = {Ke:.3f};  K_eff/2 = {Ke/2:.3f}.  Read D_x/(Keff/2) and D_y/Keff as the")
    print("scheme factor f; both -> the clean constant as eps=g h k^2/r_f^2 -> 0.")
    print("-" * 100)
    fmt = "{:>12} {:>4} {:>8} {:>9} {:>11} {:>10} {:>10}"
    print(fmt.format("orient", "m", "eps", "D_meas", "D/(Keff/2)", "D/Keff", "ck/1.667"))
    res = {}
    for orient, mx_my in [("streamwise", [(1, 0), (2, 0), (3, 0)]),
                          ("transverse", [(0, 1), (0, 2), (0, 3)])]:
        for mx, my in mx_my:
            r = run_case(h0, u0, n, L, N, mx, my)
            res[(mx, my)] = r
            print(fmt.format(orient, max(mx, my), f"{r['eps']:.3f}",
                             f"{r['D_meas']:.4g}", f"{r['D_meas']/(Ke/2):.3f}",
                             f"{r['D_meas']/Ke:.3f}", f"{r['ck_meas']/(5/3):.3f}"))
    print("-" * 100)
    # cleanest constants at the lowest mode (smallest eps)
    dx1, dy1 = res[(1, 0)]["D_meas"], res[(0, 1)]["D_meas"]
    print(f"Lowest-mode (m=1, smallest eps) constants:")
    print(f"  streamwise  D_x/(K_eff/2) = {dx1/(Ke/2):.3f}   (1D prototype gave ~0.71)")
    print(f"  transverse  D_y/(K_eff)   = {dy1/Ke:.3f}")
    print(f"  anisotropy  D_y/D_x       = {dy1/dx1:.3f}   (leading-order prediction 2.0)")
    rd = run_case(h0, u0, n, L, N, 2, 2)
    pred = dx1 * rd["kx"] ** 2 + dy1 * rd["ky"] ** 2   # tensor from m=1 constants
    print(f"  diagonal (m=2,2): gamma={rd['gamma']:.4g} vs D_x kx^2+D_y ky^2="
          f"{pred:.4g}  ratio={rd['gamma']/pred:.3f}")


if __name__ == "__main__":
    main()
