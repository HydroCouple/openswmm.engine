# The Deviation Operator under the Explicit Local-Inertial Marcher

Status: normative derivation, 2026-07-31. This is the marcher-era companion to
`DEVIATION_FORM.md` (which derived the deviation-form ROM against the retired
CVODE diffusion-wave solver). It derives, from the exact `inertialFaceUpdate`
kernel, the linear operator that governs how an ensemble **deviation** `δh`
evolves under the explicit local-inertial marcher — the operator the 2D ROM must
represent after the CVODE/ARKODE retirement (`swmm6_rel` `1e531a8a`).

Numerically confirmed by
`docs/uncertainty/prototypes/local_inertial_decay.py` (1D) and
`local_inertial_decay_2d.py` (2D). Consumed by
`P3_2D_REHOME_SPEC.md` (W1/W2/W3).

Notation: `g` gravity; `h` depth; `η = z + h` free surface; `n` Manning; `q`
unit-width discharge; `u = q/h`; `c = √(gh)` gravity-wave celerity;
`Fr = u/c`; `k` mode wavenumber; `λ_j` graph-Laplacian eigenvalue;
`δ(·)` an ensemble member's deviation from the deterministic run.

---

## 1. The scheme

The marcher is the de Almeida & Bates (2013) local-inertial face update
(`InertialKernels.hpp`), per unit-width face discharge `q` normal to a face with
flow depth `h_f` and surface slope `S_f = ∂η/∂x`:

```
q^{n+1} = ( q̂ − g·h_f·Δt·S_f ) / ( 1 + g·Δt·n²·|q⃗| / h_f^{7/3} )        (1)
```

with continuity `∂h/∂t = −∂q/∂x`. `q̂` is the θ-lateral-average (θ=1 ⇒ Bates
2010; we take θ=1 for the linear analysis). The friction is **semi-implicit**
(the denominator) with the friction magnitude taken as the **flow-vector
magnitude** `|q⃗|` (Perot reconstruction), not `|q_n|` — this choice is what
makes the 2D operator anisotropic (§4).

Define the friction relaxation rate

```
r_f = g·n²·|q| / h_f^{7/3}          [1/s]                                 (2)
```

so the continuous-time momentum balance implied by (1) is
`dq/dt = −g·h_f·S_f − r_f·q`.

---

## 2. 1D linearization

Linearize (1)+continuity about a uniform Manning base flow `(h_0, q_0)`,
`u_0 = q_0/h_0`. For a Fourier mode `∝ e^{ikx}`:

```
d(δĥ)/dt = −ik·δq̂
d(δq̂)/dt = −g·h_0·ik·δĥ − r_f·δq̂                                        (3)
```

giving the 2×2 system with characteristic equation

```
s² + r_f·s + g·h_0·k² = 0.                                               (4)
```

### 2.1 Two roots, two timescales

```
s± = ½[ −r_f ± √(r_f² − 4·g·h_0·k²) ].                                   (5)
```

- **Fast root** `s₋ ≈ −r_f`: momentum relaxation. Slaved away when `r_f`
  dominates.
- **Slow root** (small `k`, i.e. `r_f² ≫ 4 g h_0 k²`):

```
s₊ ≈ −(g·h_0 / r_f)·k²  ≡  −K_eff·k²,     K_eff = g·h_0/r_f = h_0^{10/3}/(n²·q_0).  (6)
```

**This is the collapse:** in the friction-dominated regime the deviation obeys a
pure **diffusion** law with rate `K_eff·k²` — the same `D·λ` form the old CVODE
ROM used (`k² → λ_j` on the mesh). And

```
K_eff = h_0^{10/3}/(n²·q_0) = 2·D_classic,   D_classic = h_0^{5/3}/(2n√S),     (7)
```

i.e. exactly **twice** the textbook diffusion-wave diffusivity, because the
semi-implicit denominator freezes `|q|` (linearized friction rate `r_f`, not the
full `2 r_f`).

### 2.2 The advection term (the piece the old symmetric ROM dropped)

The full friction linearization keeps the depth (`h_f`) sensitivity of the flux.
Carrying it through, the slaved flux perturbation is

```
δq = −(K_eff/2)·∂(δh)/∂x  +  (5/3)·u_0·δh,                                (8)
```

so continuity gives an **advection–diffusion** equation:

```
∂(δh)/∂t = D·∂²(δh)/∂x²  −  c_k·∂(δh)/∂x,
  D   = K_eff/2 = h_0^{10/3}/(2n²q_0),
  c_k = (5/3)·u_0        [Manning kinematic-wave celerity].               (9)
```

The old CVODE ROM used a **symmetric** graph-Laplacian → pure diffusion → it
dropped the skew `c_k` term. Physically `c_k = (5/3)u` makes deviation bands
**travel with the flow**, not merely spread. (This is the concrete face of the
checklist's H3 remark that "the skew lives in the gap, not in H".)

### 2.3 Validity: when the collapse holds

The correction to (6)/(9) is governed by the dimensionless ratio of the
gravity-wave frequency `c·k` to the friction rate `r_f`:

```
Λ = r_f/(c·k),     ε = (c·k/r_f)² = g·h·k²/r_f² = 1/Λ².                  (10)
```

- `Λ ≫ 1` (strong friction / wetted / low mode-`k`): overdamped, the
  advection–diffusion collapse is accurate; correction `O(ε)`.
- `Λ ≲ 1`: (5) goes complex → damped **gravity waves** (`Re s = −r_f/2`,
  `Im s = ±√(g h k² − r_f²/4)`). The diffusion picture fails — the *same*
  regime (thin films, dry fronts, spin-up-from-zero) where the old ROM was
  already untrustworthy.

The ROM's operative regime — wetted domains, smooth low modes — sits at `Λ ≫ 1`,
exactly PR-10's "saturated regime".

---

## 3. 1D numerical confirmation

`local_inertial_decay.py` (exact kernel, sustained uniform flow, slow-manifold
seed, complex projection separating envelope-decay from phase-speed), sweeping
`Λ` via roughness at fixed subcritical `Fr = 0.32`:

| quantity | prediction | measured (`Λ ≳ 3`) |
|---|---|---|
| diffusion `D` | `K_eff/2` (functional form) | k-independent; **0.71·(K_eff/2)** |
| advection `c_k` | `(5/3)u` | **(5/3)u to <1%**, k-independent |

The `0.71` is a scheme-specific O(1) factor (from `h_f = max(η)` face
reconstruction + semi-implicit `|q|` lag) — a constant textbook theory cannot
supply; it must be measured. The **form** is exactly as clean as the old
operator.

---

## 4. 2D extension: flow-aligned anisotropy

In 2D the friction uses the flow-**vector** magnitude `|q⃗| = √(q_x²+q_y²)`
(`InertialKernels.hpp` is explicit that this, not `|q_n|`, is required — else a
triangulated slope checkerboards). Linearizing about a base flow `u_0` in `+x`
(`q_y = 0`, `|q⃗| = q_0`):

```
streamwise (x-face):  ∂(q_x|q⃗|)/∂q_x = |q⃗| + q_x²/|q⃗| = 2q_0  → rate 2 r_f
transverse (y-face):  ∂(q_y|q⃗|)/∂q_y = |q⃗| + q_y²/|q⃗| =  q_0  → rate   r_f     (11)
```

(the cross terms `∂/∂q_other` vanish at the base). Hence the slaved diffusion
`D = g h / r_lin` is **direction-dependent**:

```
D∥ = g h/(2 r_f) = K_eff/2   (streamwise),
D⊥ = g h/ r_f    = K_eff     (transverse),      D⊥/D∥ = 2,               (12)
```

and the deviation operator is a **flow-aligned anisotropic advection–diffusion**:

```
∂(δh)/∂t = D∥·∂²∥(δh) + D⊥·∂²⊥(δh) − c_k·∂∥(δh),   c_k = (5/3)u   (∥ only).  (13)
```

### 4.1 2D numerical confirmation

`local_inertial_decay_2d.py` (structured 96², Perot + vector friction,
extrapolated to `ε → 0`):

| quantity | prediction | measured |
|---|---|---|
| `D∥` streamwise | `K_eff/2` (form) | k-independent, **0.62·(K_eff/2) = 0.31·K_eff** |
| `D⊥` transverse | `K_eff` (form) | k-independent, **1.00·K_eff** |
| anisotropy `D⊥/D∥` | ≥ 2 | **≈ 3.2** |
| `c_k` (streamwise) | `(5/3)u` | **(5/3)u to <1%**; transverse ≡ 0 |
| diagonal mode | `D∥kx²+D⊥ky²` | reproduced to **4%** |

The physical anisotropy is the factor **2** of (12); the measured **3.2** is that,
amplified by the scheme damping streamwise diffusion below its ideal
(`0.62·` vs transverse `1.00·`) — again from the `max`-reconstruction and
semi-implicit lag acting along the flow. **The magnitudes are empirical (they
will shift on unstructured triangles + VFR + θ<1); the structure is not.**

---

## 5. Consequence for the ROM: one reduced operator, three physics knobs

The clean way to carry (13) into the ROM — **without** a flow-dependent
re-eigensolve — is a Galerkin projection onto the fixed geometric-Laplacian
eigenbasis `P` (n×k, built once = W1):

```
δa = Pᵀ(h − h_det)        (reduced coordinates, k ≈ 24)
M  = Pᵀ · L_op · P        (k×k reduced operator, assembled from the flow field)
d(δa)/dt = −M·δa + forcing,    δa(t+Δt) = exp(−M·Δt)·δa(t) + …            (14)
```

`exp(−M Δt)` is a **k×k** matrix exponential (microseconds for k~24), recomputed
only when the flow field changes materially (the basis-update cadence). Every
physics rung is just *what you assemble into `M`*:

| rung | `M` | note |
|---|---|---|
| isotropic (old ROM) | `diag(D·λ_j)` | validation baseline (`D⊥/D∥ = 1`) |
| **anisotropic** (target) | `Pᵀ(D∥∂²∥ + D⊥∂²⊥)P` — dense symmetric | flow-aligned tensor; edge conductances by edge–flow angle |
| + advection | add skew `Pᵀ(−c_k ∂∥)P` | bands translate; `c_k=(5/3)u` |

So the **eigenbasis stays build-once**, the anisotropic operator is not a second
implementation — it is a richer `M`, and the isotropic case falls out at
`D∥ = D⊥`. `D∥`, `D⊥`, `c_k` are **parameters calibrated by W3** against the real
marcher MC (`test_2d_rom_marcher_coverage.cpp`), not hard-coded from §4.

Non-intrusiveness is preserved by construction: `M` and `P` read only mesh + flow
state; the deterministic run is untouched (bit-identical ROM on/off).

---

## 6. Open constants (what W3 pins)

1. The streamwise scheme factor on `D∥` (1D: 0.71; 2D structured: 0.62) on the
   real unstructured/VFR mesh.
2. The transverse factor on `D⊥` (structured: ~1.0) likewise.
3. Whether the skew advection `c_k=(5/3)u` is needed for coverage, or diffusion
   alone suffices (fixture-dependent: channelized flow vs. floodplain sheet).
4. The `Λ`-threshold below which to fall back / widen (thin-film, dry-front
   regime) — expected near the old ROM's saturated-regime boundary.

All four are measured, not tuned: the band tests are never loosened; the operator
constants are re-derived from MC (the hard rule from `VALIDATION.md`).
