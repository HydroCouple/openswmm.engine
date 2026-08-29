@page dev_cheb_section The Chebyshev-Compressed Cross-Section

@tableofcontents

# The Chebyshev-compressed cross-section: design notes

This document explains the design behind `POLYGON` cross-sections and the
`[OPTIONS] XSECT_GEOMETRY EXACT` mode: `src/engine/hydraulics/XSectBoundary.{hpp,cpp}`
(exact arc/line boundary geometry) and `src/engine/hydraulics/ChebSection.{hpp,cpp}`
(the piecewise-Chebyshev compiler that makes that geometry cheap to evaluate
in the hot loop). It is written for a maintainer who needs to know *why* the
code is shaped the way it is, not just what it does — the Doxygen comments
in the headers cover the latter.

## 1. Why an arc/line boundary gives exact A, P, B, and I₁

A cross-section's wetted area, wetted perimeter, top width, and first moment
of area at a given depth `y` are all properties of the closed curve that
forms the section's boundary, clipped at height `y`. If that boundary is a
chain of straight segments and circular arcs, every one of those properties
has a closed form — there is nothing left to discretize.

**Area and first moment (Green's theorem).** For a closed, counter-clockwise
curve, the enclosed area is the line integral `A = (1/2) ∮ (x dy − y dx)`,
and the first moment about `y = 0` is `M_y = ∮ (−1/2) y² dx`. Both integrals
have closed forms for the two element types used here:

```
greenArea(segment)  = 0.5*(x0*y1 − x1*y0)
greenArea(arc)      = 0.5*( r²(a1−a0) + r*( cx*(sin a1 − sin a0)
                                           − cy*(cos a1 − cos a0) ) )

greenMomentY(segment) = -0.5*(x1−x0)*(y0² + y0*y1 + y1²)/3
greenMomentY(arc)     = (r/2)*[ F(a1) − F(a0) ],
   F(t) = cy²*(−cos t) + 2*cy*r*(t/2 − sin(2t)/4) + r²*(−cos t + cos³t/3)
```

Summing these over every element clipped below `y`, plus a synthetic
horizontal segment closing each wetted span at the free surface (needed so
the sum is over closed loops — see `evalExact`'s own header comment), gives
the exact area and first moment at any depth, in closed form, for any chain
of lines and arcs. No sampling, no discretization error: the only error in
the pipeline is the Chebyshev *fit* of this exact function, which is a
separate and controllable choice (§3).

**Perimeter.** `arcLength(segment) = |p1 − p0|`; `arcLength(arc) = r·|a1−a0|`.
Both exact.

**Top width.** The crossings of the boundary with a horizontal line at
height `y` are found in closed form too: for a segment, linear interpolation
of `t` where `y0 + t(y1−y0) = y`; for an arc, `theta = asin((y−cy)/r)` (and
its reflection `π−theta`) directly inverts the circle equation
`cy + r·sin(theta) = y`. No root-finding — `arcCrossingAngles` is one
`asin` call. Width is the sum of the alternating spans between the sorted
crossings.

This is the entire reason `POLYGON` exists: every one of EPA SWMM's built-in
rounded shapes (EGG, HORSESHOE, GOTHIC, ...) is instead defined by a uniform
table of 21 to 51 points (depending on the shape and the field), with
*linear* interpolation between points — and that interpolation carries real,
measured error, worst at low fill: the 51-point CUSTOM table
is off by up to 41% in area below 5% of full depth (1.4% above it); the
51-point CIRCULAR table's worst relative area error below a tenth of full
depth measures 470% against the analytic circle (test-pinned in
`test_fv_solver_closure.cpp`, where the compiled boundary's worst over the
same band is 4.5e-7); EGG runs 4–5% off even at ordinary mid-range depths;
and GOTHIC's own width and area tables disagree with *each other* by up to
439% near the invert. An arc/line boundary has no interpolation step to
carry any of that error.

## 2. Why I₁ needs no depth quadrature

The Preissmann/FV solvers need `I₁(y) = ∫₀ʸ A(η) dη`, the first moment of
area with respect to depth (not to be confused with `M_y` above, the first
moment with respect to `x = 0` that Green's theorem gives directly). The
textbook way to get it is a numerical quadrature over depth — SWMM's own
`buildI1Table` does exactly that, with Simpson's rule.

That quadrature is unnecessary here because of a simple identity: the first
moment of area about the free surface, `∫∫(y − η) dA`, equals `A·(y − ȳ)`,
where `ȳ` is the area centroid depth — and `ȳ` is *already* exactly
computed by `M_y / A` from the same Green's-theorem sum area/moment
computation above. So:

```
I₁(y) = A(y) · (y − ȳ(y))
```

with `ȳ(y) = M_y(y) / A(y)`, both already exact and already computed for
free. `ExactProps::i1` is set this way directly in `evalExact` — one
division and one subtraction, not an integral. This is also why `Phase 6`'s
FV wiring replaces `buildI1Table`'s Simpson quadrature with a closed form
built the same way (`NetworkMeshBuilder.cpp`'s `exactI1`): Simpson's error
term is largest exactly where `A ~ y^1.5` has an unbounded fourth
derivative (near a round invert), which is precisely the regime a closed
form has no error term to blow up in.

## 3. Why splitting at critical heights + a coordinate change gives geometric
   (not merely fast) convergence

A Chebyshev series converges *geometrically* — error shrinking like `ρ⁻ⁿ`
for some `ρ > 1` — if and only if the function being fit is analytic
(infinitely differentiable, with a convergent power series) on an open
region of the complex plane containing the fit interval. It converges only
*algebraically* — error shrinking like `n⁻k` for some finite `k` — if the
function has a singularity (a branch point, a kink) sitting on or near the
interval.

`A(y)` for a real cross-section has two structural sources of exactly this
kind of singularity, both handled by construction rather than by adding
more coefficients:

- **A smooth horizontal tangency** (a round invert or crown) puts a square-
  root branch point in `A(y)`: near such a point, `A − A₀ ~ c·|y − y₀|^1.5`.
  Fit directly in `y`, this is a `y^1.5` term — the second derivative blows
  up, and the series can only converge algebraically (`Phase 4`'s "1.9e-4
  stalled at n=20" identity-map measurement is exactly this failure mode).
  The fix is to fit in a *stretched* coordinate `u` where the map is chosen
  so `A` becomes analytic in `u` — see the coordinate-map table below —
  which is a coordinate change, not smoothing or approximation: `A(y(u))`
  is bit-for-bit the same function, evaluated along a reparametrized axis
  where its power series actually converges.
- **A corner, a bench, or a curvature jump** does not put a branch point in
  `A` itself (a corner is `C⁰` and possibly not `C¹`, but `A(y)` is the
  *integral* of the width, which only needs the width to be continuous for
  `A` to stay analytic through it) — but a table-driven fit spanning such a
  point would still blur it, because a single global series has to
  represent both sides with the same coefficients. Splitting into a
  separate piece on each side removes the problem at its source: each
  piece's own boundary is where the split happens, so nothing is asked to
  represent behavior it cannot.

`findCriticalHeights` (`XSectBoundary.cpp`) locates every point where either
of these can occur — every element endpoint (covers corners, benches, and
curvature jumps) and every arc's horizontal tangency (covers smooth round
inverts and crowns) — and tags each side of it with a Puiseux exponent,
`1.0` (analytic) or `1.5` (square-root branch). `ChebSection::compile()`
then fits one Chebyshev series per interval between consecutive critical
heights, in the coordinate the exponent pair calls for:

| exp at low end | exp at high end | `y(u)` (normalized) | `u(s)` |
|---|---|---|---|
| 1.5 | 1.5 | `(1 − cos(πu))/2` | `acos(1 − 2s)/π` |
| 1.5 | 1.0 | `u²` | `√s` |
| 1.0 | 1.5 | `1 − (1−u)²` | `1 − √(1−s)` |
| 1.0 | 1.0 | `u` | `s` |

(`s = (y − y_lo)/(y_hi − y_lo)`.) A piece singular at *both* ends needs the
two-sided `1.5/1.5` row, whose map is `acos` — measurably the most
expensive transcendental in the whole evaluation path (§6). `compile()`
therefore enforces a stronger invariant than the table alone requires: **no
compiled piece may be singular at both ends.** A piece that would be is
split at any interior point first (§4's Reeb-graph framing: critical
heights are nodes, and no edge may have two singular endpoints) — after
which only the `1.5/1.0` and `1.0/1.5` rows are ever used for a singular
end, and their map is a hardware `sqrt`, not `acos`. This removes the two-
sided row from the evaluation path entirely without giving up geometric
convergence: splitting a piece never removes a singularity, but it does
guarantee every remaining piece has at most one.

**The comparison the design was made from:** for a "benched" section (a box
sitting on a semicircular low-flow channel — a bench corner plus a round
invert in one profile), 33 Chebyshev coefficients across the split pieces
reach `2.1e-12` relative error in `A`. A single global fit of the same
profile with 129 PCHIP knots — a competent shape-preserving spline, not a
strawman — reaches only `2.8e-3`: first order, because the spline is forced
to straddle both the bench kink and the `y^1.5` invert behavior with no
mechanism to remove either. `test_cheb_section.cpp`'s
`BenchedSectionNeedsTheCriticalHeightSplit_TrapSetter` pins the split
version against an un-split fit of the *same total coefficient budget on
the same profile*, so a future refactor that quietly removed the splitting
logic would fail a test immediately rather than only showing up as a slow
accuracy regression on real models.

## 4. What `rho_a` means

`bernsteinRho` estimates the parameter `ρ` of the Bernstein ellipse the fit
function is analytic inside: an ellipse in the complex plane with foci at
`±1` and semi-axis sum `ρ`. It is read directly off the fitted
coefficients' decay rate, `|c_k| ~ ρ⁻ᵏ`, by a least-squares fit to
`log|c_k|` vs. `k` over the coefficients above the numerical noise floor.

- `ρ > 1`: the function is analytic on a genuine neighborhood of the fit
  interval, and the series converges geometrically. Larger `ρ` means a
  wider such neighborhood and faster convergence.
- `ρ → 1`: a singularity sits on or very near the interval, and convergence
  degrades to algebraic. This is the compiler's *dynamic* signal that a
  piece needs subdividing, above and beyond the *static* signal
  `findCriticalHeights` already gives — it catches cases the critical-height
  enumeration cannot see in advance, such as an interval that happens to
  need more resolution purely from its own aspect ratio.

The one subtlety worth documenting: `ρ_a` is meaningless, not merely small,
for a function with structural zero coefficients — an even or odd function
(common for a shape symmetric about its own vertical axis at some sub-
range) has every other coefficient exactly zero, and a naive log-fit
through a zero returns `NaN`. `bernsteinRho` fits only through coefficients
above the noise floor for this reason; validated against
`1/(x² + a²)` (poles at `x = ±ia`, so `ρ = a + √(a²+1)` in closed form),
which is also even, so this validation doubles as the regression test for
that trap.

## 5. Depth conservation on a run-time geometry change

`INetworkSolver::refreshConduitGeometry` (Phase 7) lets a conduit's
cross-section change mid-run — sediment deposition, corrosion, a CIPP
liner — and the caller chooses how the water already in the conduit
reconciles against the new boundary:

- **`CONSERVE_DEPTH`**: `cell_a_new = areaOfDepth(g_new, depthOfArea(g_old, cell_a_old))`.
  The free surface stays at the same *elevation*; whatever volume the new
  boundary can no longer hold at that depth is displaced and removed from
  the conduit. Correct when solid material intrudes into the flow (the bed
  or wall physically displaces water).
- **`CONSERVE_VOLUME`**: `cell_a_new = cell_a_old` unchanged; the depth is
  whatever the new closure says that same area corresponds to. Correct when
  material is removed (the same water now occupies a larger section, so the
  free surface drops, or — if the section shrank — rises and potentially
  surcharges; `areaOfDepth` is defined above the crown for every section via
  the Preissmann slot, so `CONSERVE_VOLUME` on a shrinking section is always
  well-defined, never a "no depth exists" failure).

The displaced volume `refreshConduitGeometry` returns for `CONSERVE_DEPTH`
is `Σ (a_old − a_new)·dx·barrels` over the conduit's cells — booked by the
*caller* into whatever continuity ledger the calling context uses (the
function itself does not write to the routing mass balance; see the C API
Doxygen for `swmm_link_set_polygon`). A datum subtlety worth remembering
for a future change here: `fromPolyline`/`fromArcSpec` normalize the
supplied boundary so its own lowest point sits at `y = 0`, which silently
discards where that boundary actually sat relative to the *old* invert. A
sediment bed sits *above* the original invert, so the flow bed itself
rises by that amount; `FvGeometry::bed_offset` captures the shift, and
`CONSERVE_DEPTH` is defined in terms of free-surface *elevation*
(`h_new = h_old − Δbed_offset`), not raw numeric depth, specifically so
this case is handled correctly. Getting this backwards is not something a
test conveniently catches by symptom — the wrong sign reads as *added*
capacity from a sediment fill that should be removing it, which is why the
sign of the displaced volume is the thing worth checking by hand whenever
this code is touched.

## 6. Measured performance

The numbers below are the ones the original design was made from — a C
microbenchmark and a Python prototype of the compiler, 4M evaluations of
four fields on one core, synthetic (not real-shape) coefficients:

| comparison | result |
|---|---|
| Chebyshev deg-8 fused vs. 512-point table | ~70 ms vs. ~96 ms (~25–30% faster), ~7 orders better accuracy |
| Chebyshev deg-12 / deg-16 | ~81 ms / ~106 ms |
| 65,536-point table (cache-bound) | ~161 ms |
| Analytic circular path, ONE field (`acos`/`sin`) | ~91 ms |
| Rejected: cache the piece index per cell | 136 ms vs. 51 ms uncached — table memory traffic swamps the saved compare |
| Rejected: sort cells by piece for vectorization | 144 ms — needs multiple passes over all cells |
| Benched-box accuracy: 33 Chebyshev coeff. vs. 129 PCHIP knots | `2.1e-12` vs. `2.8e-3` relative error in `A` |

**What was actually measured once real (not synthetic) compiled sections
went through the real engine, on the Bellinge network (953 conduits, ~70%
CIRCULAR) — because the synthetic numbers above turned out not to
generalize to every shape:**

- A *round* section (a true circle) is the one shape where the design's
  25–30% advantage does not appear: it needs the two-sided `acos` map at
  both ends until normalized (§3), and even after normalization and every
  later fusion/packing optimization, the batched EXACT path reaches only
  **near-parity** with the legacy table (~1.08–1.15×), not a clear win —
  because the sqrt map itself costs real cycles a straight-line table
  lookup never pays. `test_cheb_section.cpp`'s `EvaluationCostAgainstTable
  LookupAndTheAnalyticPath_Informational` measures this directly and
  documents it as expected, not a regression.
- A shape with a **pointed** crown or invert (GOTHIC, a V-notch) is
  analytic there rather than square-root, needs no `acos` at all, and
  measured **5.2× faster** than its own legacy table lookup on Bellinge
  (399 ms → 77 ms for 4M evaluations) — because GOTHIC's legacy accessor
  has no direct table for one field and is pathologically slow, not because
  the compiled path is unusually fast.
- Fixing genuinely redundant evaluation (four independent piece-scans
  collapsed into one shared-basis pass; a runtime Newton solve for the
  inverse `y(A)` compiled at load time instead) closed a **5.64× → ~1.13×**
  EXACT-vs-LEGACY gap on Bellinge under DYNWAVE over several dedicated
  passes (final contemporaneous interleaved pair 40.97 s vs 36.31 s; this
  machine shows up to ±18% run-to-run spread, so treat the last few percent
  as noise) — none of which changed a single output value, only which work
  was redundant. The largest single item, at 49.1% of a profiled run, was the
  runtime Newton inversion; compiling it ahead of time (the same coordinate-
  change idea as §3, applied to the *inverse* function `u(A)` instead of
  `A(y)`) took it to 0.7% of the profile.

The honest summary, restated from the top-level design document: **this is
an accuracy-and-architecture project that happens not to cost performance**
for most shapes, and reaches rough parity rather than a clear loss for the
one shape (a true circle) where the geometry itself is least favorable to a
polynomial series. It was never a performance project, and Phase 0 of the
plan existed specifically to confirm that premise before anything was built.

## 7. Known limits

Stated plainly, not as caveats to be read past:

- **Resistance is still `R = A/P`** — a single Manning's `n` applies around
  the entire wetted perimeter. There is no composite-roughness treatment
  that varies `n` by which boundary element it is on (a channel with a
  different roughness on its bed than its banks, for example). A future
  extension could carry a Manning's `n` per `BElem` and compute a discharge-
  weighted composite, but nothing in the current design does this.
- **The formulation is 1-D.** Cross-flow, secondary circulation, and other
  genuinely 2-D effects inside a single cross-section are outside its
  scope, the same as every other shape this engine supports.
- **Only circular arcs are exact.** A true ellipse's perimeter has no
  elementary closed form (it needs an elliptic integral); this boundary
  representation supports lines and *circular* arcs only, both exactly, so
  an elliptical section built through `POLYGON` must be approximated as a
  polyline (or a chain of circular arcs) rather than represented exactly.
  This is a real accuracy trade for that one case, unlike everything else
  in this document.
- **The section is uniform along the conduit's full length.** There is no
  representation of a boundary that tapers from one end of a conduit to the
  other, and consequently no `I₂` shape-variation source term (the momentum
  source that a genuinely tapered conduit would need). A conduit whose
  cross-section changes size along its length must be modeled as multiple
  shorter conduits, each with its own uniform section.
- **Run-time geometry change is FV-only** (`INetworkSolver::
  refreshConduitGeometry`, §5): the legacy DYNWAVE/KINWAVE solvers hold
  their own per-link state with no defined mid-Picard re-seed, so
  `swmm_link_set_polygon` is refused outright (`SWMM_ERR_GEOMETRY`) unless
  FV routing is active. It is also **not carried in hotstart files** — a
  hotstart saved after a run-time geometry change does not serialize the
  modified boundary, so reloading it restores the `.inp` file's original
  shape at the saved run's depths. `SAVE HOTSTART` emits a warning when
  this applies; neither gap is silent.
