#!/usr/bin/env python3
"""
Prototype: does the local-inertial marcher's deviation-decay operator collapse
to the old diffusion ROM's  K_eff * lambda_j ?

Context
-------
swmm6_rel retired CVODE/ARKODE; the 2D solver is now the de Almeida & Bates
(2013) explicit local-inertial FV marcher (see InertialKernels.hpp). The old 2D
ROM propagated member deviations with  d(da_j)/dt = -K_eff * lambda_j * da_j,
K_eff a Manning diffusivity, lambda_j graph-Laplacian eigenvalues. Question for
P3_2D_REHOME_SPEC.md W3: under the new marcher, does that operator still hold,
and does it collapse to something as clean as the old form?

Analysis (per-face, unit width, theta=1 -> Bates 2010)
------------------------------------------------------
Momentum (semi-implicit friction, |q| lagged as the scheme does):
    dq/dt = -g*h_f*S_f - r_f*q ,   r_f = g*n^2*|q| / h_f^(7/3)   [1/s]
Continuity (1 mode, wavenumber k):  d(dh)/dt = -d(dq)/dx  ->  = -i k dq
Momentum (bed fixed):               d(dq)/dt = -g*h_f*(i k) dh - r_f dq

2x2 system  d/dt[dh;dq] = [[0, -ik],[ -g h ik, -r_f]] [dh;dq]
    characteristic:  s^2 + r_f s + g h k^2 = 0
    slow root (k small):  s+  ~  -(g h / r_f) k^2  ==  -K_eff k^2
        with   K_eff = g h / r_f = h^(10/3) / (n^2 q0)         [m^2/s]
    fast root:            s-  ~  -r_f   (momentum relaxation, slaved away)

=> In the friction-dominated (overdamped) regime the deviation obeys EXACTLY the
   old diffusion law, rate  K_eff * k^2  (k^2 -> lambda_j on the mesh).
   K_eff = h^(10/3)/(n^2 q0) = 2 * [ h^(5/3)/(2 n sqrt(S)) ] = 2 * D_classic,
   i.e. twice the textbook Manning diffusivity because the scheme freezes |q|
   in the semi-implicit denominator (linearized friction rate r_f, not 2 r_f).
   THIS factor is the only recalibration W3 must pin.

Governing dimensionless number (overdamped vs. gravity-wave):
    discriminant  r_f^2  vs  4 g h k^2
    Lam = r_f / (c k),  c = sqrt(g h)   (friction rate / gravity-wave frequency)
    overdamped/diffusive when Lam >> 1;  correction to K_eff*k^2 is O(1/Lam^2).
    Exact slow root:  gamma_exact = (r_f - sqrt(r_f^2 - 4 g h k^2)) / 2  (real),
                      or complex (damped gravity wave) when 4 g h k^2 > r_f^2.

This script implements the EXACT kernel update on a periodic 1D uniform base
flow, seeds a cosine depth deviation ON the slow manifold, and measures the
decay rate, comparing to K_eff*k^2 (clean diffusion) and to the exact 2x2 root.
"""
import numpy as np

G = 9.80665


def kernel_face_q(q, hf, dt, Sf, n2, qmag, fr_max=1.5):
    """Exact InertialKernels.hpp inertialFaceUpdate + froudeCap (theta=1)."""
    h73 = hf * hf * np.cbrt(hf)
    num = q - G * hf * dt * Sf
    den = 1.0 + G * dt * n2 * qmag / h73
    qstar = num / den
    qcap = fr_max * hf * np.sqrt(G * hf)      # froudeCap: |q| <= Fr_max h sqrt(g h)
    return np.clip(qstar, -qcap, qcap)


def run_case(h0, u0, n, Lx, N, mode_m, seed_amp=1e-4):
    """One periodic uniform-flow case; returns measured/predicted decay rates."""
    dx = Lx / N
    x = (np.arange(N) + 0.5) * dx
    q0 = u0 * h0
    k = 2.0 * np.pi * mode_m / Lx
    c = np.sqrt(G * h0)
    r_f = G * n * n * abs(q0) / h0 ** (7.0 / 3.0)
    K_eff = h0 ** (10.0 / 3.0) / (n * n * q0) if q0 > 0 else np.inf   # = g h / r_f
    # Constant driving slope that exactly balances base friction -> the base is a
    # genuine, PERSISTENT uniform Manning flow (g h S_drive = r_f q0). Without
    # it a flat periodic base decelerates to rest within the measurement window.
    S_drive = r_f * q0 / (G * h0)

    # Full friction linearization -> advection-diffusion, NOT pure diffusion:
    #   d(dh)/dt = D d2(dh)/dx2 - c_k d(dh)/dx,
    #   D   = K_eff/2 = h^(10/3)/(2 n^2 q0)   (the 2 from d|q|q/dq = 2|q|)
    #   c_k = (5/3) u0                        (Manning kinematic-wave celerity)
    D_pred = 0.5 * K_eff                            # diffusivity (envelope decay)
    ck_pred = (5.0 / 3.0) * u0                      # advection speed (phase)
    gamma_diff = D_pred * k * k                     # predicted envelope decay
    Lam = r_f / (c * k)

    # --- faithful marcher: cells h[i], faces qf[i] between cell i and i+1 (periodic)
    h = np.full(N, h0)
    # deviation seeded on the slow manifold: dq_face = -K_eff * d(dh)/dx
    dh0 = seed_amp * h0 * np.cos(k * x)
    h = h + dh0
    ip = (np.arange(N) + 1) % N
    im = (np.arange(N) - 1) % N
    # face i sits between cell i and cell i+1; slope uses (eta_{i+1}-eta_i)/dx
    dhdx_face = (dh0[ip] - dh0) / dx
    qf = np.full(N, q0) - (K_eff if np.isfinite(K_eff) else 0.0) * dhdx_face

    # stable sub-CFL step
    dt = 0.15 * dx / c
    # window: several envelope e-folds, past the fast momentum transient (~5/r_f)
    t_end = max(6.0 / max(gamma_diff, 1e-9), 12.0 / r_f if r_f > 0 else 1e3)
    nsteps = min(400000, max(400, int(t_end / dt)))

    # COMPLEX projection: separate envelope decay (diffusion) from phase (advection).
    cos_k, sin_k = np.cos(k * x), np.sin(k * x)
    ac, as_, times = [], [], []
    for s in range(nsteps):
        etaL = h                                   # flat bed z=0 -> eta = h
        etaR = h[ip]
        hf = np.maximum(np.maximum(etaL, etaR), 1e-9)
        Sf = (etaR - etaL) / dx - S_drive          # constant driver sustains base
        qf = kernel_face_q(qf, hf, dt, Sf, n * n, np.abs(qf))
        h = h - (dt / dx) * (qf - qf[im])
        dev = h - h.mean()
        ac.append((2.0 / N) * np.dot(dev, cos_k))
        as_.append((2.0 / N) * np.dot(dev, sin_k))
        times.append((s + 1) * dt)

    ac, as_, times = np.array(ac), np.array(as_), np.array(times)
    env = np.hypot(ac, as_)                         # |a| — decays at D k^2, no aliasing
    phase = np.unwrap(np.arctan2(as_, ac))          # advances at c_k k

    # fit past the fast transient
    t_fast = 5.0 / r_f if r_f > 0 else 0.0
    lo = min(max(int(np.searchsorted(times, t_fast)), 1), nsteps - 20)
    target = env[lo] * np.exp(-3.0)
    idx = np.where(env[lo:] < target)[0]
    hi = (lo + idx[0]) if len(idx) else nsteps - 1
    if hi - lo < 20:
        hi = min(lo + max(20, nsteps // 3), nsteps - 1)
    A = np.vstack([times[lo:hi], np.ones(hi - lo)]).T
    gamma_meas = -np.linalg.lstsq(A, np.log(env[lo:hi] + 1e-300), rcond=None)[0][0]
    # advection speed from the phase slope: dphase/dt = -c_k k (sign per convention)
    ck_meas = abs(np.linalg.lstsq(A, phase[lo:hi], rcond=None)[0][0]) / k

    Fr = u0 / c
    return dict(Fr=Fr, Lam=Lam, K_eff=K_eff, D_pred=D_pred, ck_pred=ck_pred,
                gamma_diff=gamma_diff, gamma_meas=gamma_meas,
                D_meas=gamma_meas / (k * k), ck_meas=ck_meas, k=k, r_f=r_f)


def main():
    print("Local-inertial deviation operator: does it collapse to a clean form?")
    print("=" * 100)
    print("Claim: d(dh)/dt = D d2(dh)/dx2 - c_k d(dh)/dx  (ADVECTION-DIFFUSION)")
    print("  D   = h^(10/3)/(2 n^2 q0)   [Manning diffusivity, same form as old ROM K1d]")
    print("  c_k = (5/3) u0              [Manning kinematic-wave celerity]")
    print("Base SUBCRITICAL (u0=1 m/s, h0=1 m, Fr=0.32); Lx=4000 m, N=800, mode m=1.")
    print("-" * 100)
    hdr = ("n", "Lam", "D_pred", "D_meas", "D_meas/pred",
           "ck_pred", "ck_meas", "ck_meas/pred")
    print("{:>6} {:>8} {:>9} {:>9} {:>12} {:>8} {:>8} {:>13}".format(*hdr))
    for n in [0.025, 0.04, 0.07, 0.12, 0.20]:
        r = run_case(h0=1.0, u0=1.0, n=n, Lx=4000.0, N=800, mode_m=1)
        print("{:>6.3f} {:>8.2f} {:>9.4g} {:>9.4g} {:>12.3f} {:>8.4g} {:>8.4g} {:>13.3f}".format(
            n, r["Lam"], r["D_pred"], r["D_meas"], r["D_meas"] / r["D_pred"],
            r["ck_pred"], r["ck_meas"], r["ck_meas"] / r["ck_pred"]))
    print("-" * 100)
    print("Mode-number sweep (n=0.12): D should be k-INDEPENDENT (diffusion), c_k k-independent (advection):")
    print("{:>5} {:>10} {:>10} {:>12} {:>10} {:>12}".format(
        "m", "D_pred", "D_meas", "D_meas/pred", "ck_meas", "ck_meas/pred"))
    for mm in [1, 2, 3, 4]:
        r = run_case(h0=1.0, u0=1.0, n=0.12, Lx=4000.0, N=800, mode_m=mm)
        print("{:>5d} {:>10.4g} {:>10.4g} {:>12.3f} {:>10.4g} {:>12.3f}".format(
            mm, r["D_pred"], r["D_meas"], r["D_meas"] / r["D_pred"],
            r["ck_meas"], r["ck_meas"] / r["ck_pred"]))


if __name__ == "__main__":
    main()
