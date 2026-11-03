@page legacy_defects Known Defects in the Legacy SWMM 5.2.4 Solver

@tableofcontents

# Known defects in the legacy SWMM 5.2.4 solver

This page is a register of defects that OpenSWMM has **inherited from EPA SWMM
5.2.4 and deliberately does not fix**. Each entry records what is wrong, what
was measured, which code paths can reach it, and — where one exists — the
supported way to avoid it.

Nothing on this page is a bug report against OpenSWMM's own v6 code. Defects in
the v6 engine (the finite-volume solver, the compiled-boundary geometry, the
C API) are ordinary bugs and are tracked as GitHub issues, not here.

## Why these are not simply fixed

`src/legacy/` holds the SWMM 5.x solver essentially unmodified, and the
LEGACY-mode code paths in `src/engine/` are held to a **bit-parity contract**
with it: for the same input, OpenSWMM in its default configuration must
reproduce EPA SWMM's output bit for bit. That contract is what lets an existing
SWMM 5 model be re-run on OpenSWMM and have any difference in the results be
attributable to something other than the engine swap.

A bit-parity contract necessarily preserves bugs along with everything else. So
the policy is:

- **Legacy paths are documented, never corrected.** A defect here is a
  characteristic of the model that users are relying on, whether they know it
  or not; silently changing it would break the one guarantee the LEGACY mode
  exists to provide.
- **New code paths may do better.** Where OpenSWMM adds a genuinely new
  representation — most importantly `[OPTIONS] XSECT_GEOMETRY EXACT` and
  `POLYGON` cross-sections (@ref dev_cheb_section) — it is free to be more
  accurate, because a user opting into a new representation is not asking for
  bit parity with SWMM 5. Several entries below are fixed *in that sense*: the
  defect remains in LEGACY and is gone in EXACT.
- **Where a fix would need a hydraulic-convention decision rather than a
  correctness argument, it is not made unilaterally** — even on the new path.
  @ref legacy_defects_conveyance_branch is the current example.

Anything upstream ought to hear about is flagged as such in the entry.

## Reading the measurements

Several entries quote a table-consistency diagnostic. It is worth stating once
what it does and does not prove.

A table-defined shape has no external ground truth to check against — the table
*is* the shape's definition. But SWMM stores area and top width in **separate,
independently digitized tables**, and for any real cross section these are not
independent: `dA/dy` must equal `W(y)` everywhere. Comparing
`|dA/dy − W| / W` therefore bounds the table error **using no outside
reference at all**.

It understates. On CIRCULAR — the one shape with a closed-form geometry to
compare against — the diagnostic reports 0.34% where the true error is 1.3%,
roughly 4× larger. Every figure from this diagnostic is a conservative floor.

# LD-1 — Tabulated shapes' area and width tables describe different pipes {#legacy_defects_table_consistency}

**Status:** documented, not fixed. Worth raising upstream.
**Where:** `src/engine/hydraulics/xsect_tables.hpp`, `src/legacy/engine/xsect.c`.

## What is wrong

For the closed conduit shapes defined by lookup table rather than formula, the
area-vs-depth and width-vs-depth tables are not mutually consistent. For at
least one shape (`GOTHIC`) they disagree so badly that they cannot both be
describing the same physical pipe.

## Evidence

`|dA/dy − W| / W`, evaluated at `y/D = 0.02`:

| Shape | Inconsistency at y/D = 0.02 |
|---|---|
| CIRCULAR | 0.34% |
| ARCH | 8% |
| HORSESHOE | 13% |
| EGGSHAPED | 17% |
| BASKETHANDLE | 27% |
| CATENARY | 60% |
| **GOTHIC** | **226%** (worst observed over the full range: **439%**) |

Remember these are floors (see above): the true disagreement is likely ~4×
larger. A 226% inconsistency means the width implied by the area table and the
tabulated width differ by more than the width itself.

## Why the tables are shaped this way

The underlying tables are coarse, and unevenly so. Confirmed against
`xsect_tables.hpp`:

- **CIRCULAR** is the best-resourced shape: all five of its tables (A, R, W, S,
  Y) carry **51** points.
- **ARCH, BASKETHANDLE, EGGSHAPED, HORSESHOE, HORIZ_ELLIPSE, VERT_ELLIPSE**
  carry **26** points for A, R and W.
- **GOTHIC, CATENARY, SEMIELLIPTICAL, SEMICIRCULAR** have **no area table and
  no hydraulic-radius table at all** — only width (**21** points), plus the
  51-point S and Y tables. Their area and hydraulic radius are reconstructed
  indirectly, which is why `GOTHIC`'s `getRofY` has historically been both the
  slowest and the least accurate accessor in the set.

## Impact

Any LEGACY-mode result for these shapes carries this error. It is largest near
the invert, where the tables are sparsest relative to how fast the geometry
changes — which is also where shallow-flow routing spends most of its time in
dry weather.

## Mitigation

`[OPTIONS] XSECT_GEOMETRY EXACT` removes this defect by construction rather
than by re-fitting the tables. `LegacyShapeBoundary`
(`src/engine/hydraulics/LegacyShapeBoundary.{hpp,cpp}`) reconstructs the shape
as a real polyline boundary, and area, perimeter, hydraulic radius and first
moment are then all derived from *that single boundary* by Green's-theorem
integration. A and W cannot disagree afterwards, because there is only one
object left to disagree with. See @ref dev_cheb_section.

# LD-2 — Tabulated geometry is badly wrong at shallow depths {#legacy_defects_shallow_tables}

**Status:** documented, not fixed.
**Where:** `src/engine/hydraulics/xsect_tables.hpp`, `xsect::lookup`.

## What is wrong

The tables are sampled uniformly in depth and interpolated linearly. Near the
invert, a closed conduit's area varies as roughly `y^1.5` and its width as
`y^0.5` — both have unbounded derivatives at `y = 0` — so uniform linear
interpolation is at its worst exactly where the tables are least able to
afford it.

## Evidence

Measured against the analytic circular-segment area, over 999 depths:

- Below `y/D = 0.10`, the LEGACY CIRCULAR table's worst relative area error is
  **4.7 (470%)**, against **4.5×10⁻⁷** for the compiled boundary.
- Over the full depth range the compiled boundary is closer to the analytic
  answer at **99.9%** of sampled depths, with roughly **317×** lower mean
  absolute error.
- The 51-point `CUSTOM` table, for comparison, carries about **41%** error in
  the same shallow regime.

The error is not confined to the near-invert regime. For `EGGSHAPED` at
ordinary mid-range depths (`y/D` between 0.3 and 0.7), measured error against
the reconstructed exact boundary is up to **4.7%** in area and **3.6%** in
hydraulic radius, with top width the best-behaved at about **0.5%**.

## Impact

This is the single largest accuracy limitation in LEGACY-mode geometry, and it
is the reason `XSECT_GEOMETRY EXACT` exists. It matters most for dry-weather
flow, for the initial filling transient of a storm, and for any mass-balance
figure accumulated over long shallow-flow periods.

The finite-volume solver is markedly more sensitive to it than the dynamic-wave
solver, because FV embeds the geometry in its conservation statement rather
than consulting it as a lookup. On a real network (Bellinge, 953 conduits) over
a filling window, continuity error was **−4.453% under LEGACY versus −0.022%
under EXACT** — two orders of magnitude, from the geometry representation
alone. See @ref dev_cheb_section for the fuller comparison.

## Mitigation

`[OPTIONS] XSECT_GEOMETRY EXACT`, as for @ref legacy_defects_table_consistency.

# LD-3 — The conveyance inverse is discontinuous and non-monotone near full {#legacy_defects_conveyance_branch}

**Status:** documented, not fixed — in LEGACY *or* in the compiled path. The
remedy is a hydraulic-convention decision, not a correctness fix; see below.
**Where:** `generic_getAofS` and `circ_getAofS` in
`src/engine/hydraulics/XSectKernels.hpp`; the same two functions in
`src/legacy/engine/xsect.c`.

## What is wrong

A closed conduit with a narrowing crown reaches **peak conveyance before it
runs full**. Friction from the wetted perimeter keeps growing as the water
approaches the crown while the added area shrinks toward nothing, so the
section factor `S = A·(A/P)^(2/3)` rises to a maximum at `a_max < a_full` and
then falls back to `s_full`.

Consequently `S(a)` is **not injective**: every `s` in `[s_full, s_max]` has
two valid area preimages, one on the rising branch and one on the falling
branch. `getAofS` — "given a target conveyance, how full is the pipe?" — must
choose, and it chooses silently.

`generic_getAofS` brackets the search in `[a_full, a_max]` (the falling,
near-full branch) whenever `s` lands in that window, and `[0, a_max]`
otherwise. `circ_getAofS` never reaches the question, because it clamps any
`psi >= 1.0` straight to `a_full` — a different resolution of the same
ambiguity, in the same engine.

## Evidence

Measured on a compiled D = 4 ft circle:

- `a_max/a_full = 0.9743`, `y(a_max)/D = 0.938`, `s_max/s_full = 1.0757`.
- **Jump discontinuity at `s = s_full`.** At `s/s_full = 0.99999` the function
  returns `y/D = 0.8196`; at `s/s_full = 1.0` it returns `y/D = 0.9990`. An
  infinitesimal change in flow moves the answer by **0.18 D**.
- **Non-monotone above it.** As `s` continues to rise toward `s_max`, the
  returned area *decreases*, by up to **0.466% of `a_full`** — more flow
  reported as less depth.
- **Round-trip failure.** `getAofS(getSofA(a))` returns a different area for
  any `a` in the top **12.3%** of the section (`y/D` from 0.8196 to 1.0), with
  worst error **12.3% of `a_full`**, reached at `a = a_full`.

## Reachability

This is not purely theoretical, and it is not uniformly reachable either:

- **KinematicWave cannot reach it.** `KWSolver::solveConduit` clamps
  `q >= q_full` to a full section before calling, so `s < s_full` always.
- **DynamicWave can.** `computeYnorm` clamps `q` to `CD.q_max`, and
  `Routing.cpp` sets `CD.q_max = xsect_s_max * beta` — the *max-flow*
  capacity, not the full-pipe capacity. The ambiguous window is inside the
  clamp.
- **Initialization can.** `SWMMEngine`'s start-up `getDepthFromFlow`, used to
  seed depth and stored volume from a user-supplied initial flow, clamps
  nothing at all.

## Why this is not simply "fixed"

The obvious repair — always bracket `[0, a_max]`, making the inverse
continuous, monotone and round-trippable — is **wrong for the primary use
case**, and measurement is what shows it. That change would report a pipe
carrying its full design capacity as `y/D = 0.82` rather than essentially full.
For capacity and surcharge assessment that is the unsafe direction, and it
disagrees with `circ_getAofS`, which resolves the same ambiguity by clamping to
full.

The jump itself is moreover **inherent**, not incidental: `S(a)` genuinely
turns over, so any single-valued inverse whose range includes `a_full` must
jump somewhere. Only the non-monotonicity above `s_full` is straightforwardly
indefensible, and removing it means choosing between the falling-branch root
that `generic_getAofS` returns and the clamp-to-full that `circ_getAofS`
returns — a question about what the model should mean, which belongs to
maintainers and not to a geometry change.

The defect is therefore recorded in full, in this page and in a `KNOWN DEFECT`
block on `generic_getAofS` itself, and left alone pending that decision.

# LD-4 — Critical depth is solved only to about one part in 500 {#legacy_defects_ycrit}

**Status:** documented for LEGACY; **fixed for compiled boundaries**.
**Where:** `generic_getYcrit` in `src/engine/hydraulics/XSectKernels.hpp`;
`getYcritEnum` / `getYcritRidder` in `src/legacy/engine/xsect.c`.

## What is wrong

Critical depth is found by stepping through 25 equal depth increments to
bracket the root of `Q_c(y) = Q`, then closing with a **single linear
interpolation** inside the bracketing interval. That leaves an `O(y_full/25)`
discretization error regardless of how accurately `A` and `W` themselves are
known. The alternative branch, taken when the section is far from circular in
proportion, uses Ridder's method with a fixed **0.001 ft** absolute tolerance.

## Evidence

On a D = 4 ft circle (`dy = 0.16 ft`), against a true analytic critical depth
obtained by independent bisection: worst error **8.6×10⁻³ ft** — about three
orders of magnitude coarser than the 10⁻⁶ ft precision targeted elsewhere in
the geometry pipeline.

This is a property of the **solver**, not of the geometry representation: for
as long as both modes shared the routine, LEGACY and EXACT were equally coarse.

## Impact and mitigation

For LEGACY the coarse solve is arguably self-consistent — the tables feeding it
are themselves only good to ~10⁻², so refining inside one of their brackets
would be false precision — and it stays exactly as it was.

For a **compiled boundary** there is no such excuse: `chebAWofY` resolves `A`
and `W` to about `chebsec::kFitTol`. `generic_getYcrit` now polishes the
bracket the enumeration already produced, using Ridder to `kYcritTolCheb`,
whenever `XSectParams::cheb` is set. Measured on the same circle over
`q ∈ [0.25, 100]` cfs: worst error **4.1×10⁻⁸ ft**, an improvement of roughly
**2×10⁵**, which brings the original 10⁻⁶ ft target within reach with two
decades to spare.

The gate is `xs.cheb`, so LEGACY is untouched — asserted directly by
`ChebSection.CriticalDepthOnACompiledCircleMatchesTheAnalyticFormula`, which
checks that a tabulated CIRCULAR still resolves *its own* table-based `Q_c`
only to the enumeration floor. (Comparing legacy against the *analytic* root
cannot detect a leak, because the table error swamps the solver error; that
weaker form of the check was tried first and confirmed vacuous by mutation.)
