@page hydraulics_ref_ch9_two_dimensional Chapter 9: Two-Dimensional Overland Flow Analysis

@tableofcontents

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

Chapters 3, 4 and 8 all solve one-dimensional flow along a conduit. When
a sewer surcharges, the water that leaves the network spreads over the
street, ponds behind a kerb, follows the terrain rather than the pipe,
and re-enters the network somewhere else. @ref hydraulics_ref_ch2_hydraulic_model "Chapter 2"'s node-link model
has no representation for any of that: SWMM 5 either discards the
overflow or holds it in a fictitious ponded area above the node, to be
returned to the same node later.

This chapter documents the optional two-dimensional overland-flow
domain OpenSWMM provides for that water — a cell-centred finite-volume
solver for the local-inertial shallow-water equations on an unstructured
triangular mesh, coupled bidirectionally to the node-link network.

The module is activated by the presence of a mesh in the project
(`[2D_VERTICES]` and `[2D_TRIANGLES]`, inline or via `[2D_MESH_FILE]`)
rather than by a routing option. `IGNORE_2D YES` disables it while leaving the
mesh parsed and editable. The 1D network continues to route by whatever
`FLOW_ROUTING` selects; the 2D domain supplements the network routing
rather than replacing it.

## 9.1 What the method adds

**A surface that is a domain in its own right.** Ponded
volume in the node-link model belongs to the node it came from. On the
2D mesh it belongs to the terrain: it flows downhill, splits at a crown,
pools in a sag, and drains into whichever inlet it reaches. Which node
receives the water is an outcome of the calculation rather than an
input to it.

**Flow paths the network does not contain.** Overland routes — a road
acting as a channel, flow over an embankment, a flow path between two
otherwise unconnected catchments — exist in the terrain and nowhere in
the pipe topology.

**A flood extent.** The depth field over a real surface is the quantity
flood mapping needs. A ponded volume at a node is not one.

Several limitations should be noted. The method does not replace hydrology: unless
`RAINFALL_MODE` says otherwise the mesh receives the same rainfall the
subcatchments do, and both would deliver it (§9.8). It does not
infiltrate — the mesh has no soil column, and water on it leaves only by
flowing away, evaporating, or entering the network (§9.12). And its
momentum equation is an approximation of the full shallow-water
system: §9.2 states what is dropped and §9.10 measures what that
costs.

## 9.2 Governing equations

Depth-averaged mass conservation over a surface of bed elevation
\f$z(x,y)\f$ and free surface \f$\eta = z + h\f$, with unit-width discharge
\f$\mathbf{q} = h\mathbf{u}\f$ (m²/s):

| | | | |
|---|---|---|---|
| \f[\frac{\partial h}{\partial t} + \nabla \cdot \mathbf{q} = i - e + s\f] | Continuity | (9-1) | |

where \f$i\f$ is rainfall intensity, \f$e\f$ the evaporation rate and \f$s\f$ the
exchange with the 1D network, all as velocities normal to the surface.

The momentum equation solved is the **local-inertial** (or
inertial-wave) approximation of Bates et al. (2010):

| | | | |
|---|---|---|---|
| \f[\frac{\partial \mathbf{q}}{\partial t} + g\,h\,\nabla\eta + \frac{g\,n^{2}\,\lvert\mathbf{q}\rvert\,\mathbf{q}}{h^{7/3}} = 0\f] | Momentum | (9-2) | |

with \f$g = 9.80665\f$ m/s² and \f$n\f$ Manning's roughness. Compared with the
full shallow-water momentum equation, the **convective acceleration term
\f$\nabla\cdot(\mathbf{q}\mathbf{q}/h)\f$ is dropped**. Everything else —
local acceleration, the pressure gradient written as a free-surface
slope, and bed friction — is retained.

This is the single most consequential modelling decision in the chapter,
and it is a deliberate one. Retaining \f$\partial\mathbf{q}/\partial t\f$ is
what separates (9-2) from the diffusive wave: the diffusive wave has no
inertia at all, so a flood front on a flat surface propagates at a speed
set by the numerical step rather than by the physics, and the scheme
becomes stiff exactly where floods are shallow. Dropping the convective
term is what separates (9-2) from the full system, and it costs the
Bernoulli terms: the scheme cannot represent a drawdown over a crest, a
stable hydraulic jump position, or a fully supercritical profile. §9.10
shows both effects measured against closed-form solutions.

The practical justification is that urban flood flows are dominated by
gravity and friction. Bates et al. (2010) and de Almeida and Bates
(2013) delimit the regime: the local-inertial approximation is accurate
for subcritical flows over gentle slopes at Froude numbers below about
0.5, and degrades progressively as \f$Fr \to 1\f$. Flow that is
persistently supercritical, or where the momentum flux through a
contraction sets the answer, is outside it.

For steep faces the model would accelerate without bound, since nothing
in (9-2) limits the velocity a slope can generate. A Froude clamp
supplies that limit numerically (§9.5.1).

## 9.3 The computational mesh

Unlike the internal discretization of
@ref hydraulics_ref_ch8_finite_volume "Chapter 8", the mesh is an
explicit part of the model. Its cells appear in results, carry per-cell parameters,
and are addressable by the API.

Cells are triangles, listed in `[2D_TRIANGLES]` by three vertex indices
from `[2D_VERTICES]`, with Manning's \f$n\f$ and an optional initial depth
per cell:

```
[2D_VERTICES]
;;X          Y          Z        [TAG]

[2D_TRIANGLES]
;;V1  V2  V3  MANNINGS_N  [INIT_DEPTH]  [TAG]
```

Bed elevation is carried at the **vertices**. A cell's centroid
elevation \f$z_c\f$ is the mean of its three vertex elevations, so the cell
bed is the plane through them — a fact the closure of §9.4 uses
directly. Nothing in the solver reads a cell-constant bed.

Edge–neighbour adjacency is built by hashing each triangle's three
sorted vertex pairs: an edge claimed twice is interior and the two
triangles become neighbours; an edge claimed once is a domain boundary
and carries a boundary condition (§9.6). The convention throughout is
that **local edge \f$e\f$ is opposite vertex \f$e\f$**, so its endpoints are the
triangle's other two vertices. Both incident cells of an interior edge
therefore compute the same endpoint elevations, which is what makes the
face depth of §9.5.2 antisymmetric and the flux conservative.

Interior edges are then enumerated once each as **unique faces**, with
the left cell \f$L\f$, the right cell \f$R\f$, the edge length \f$\xi\f$, the
outward normal of \f$L\f$'s slot (which points \f$L \to R\f$), the edge midpoint,
and the face-normal centroid separation

| | | | |
|---|---|---|---|
| \f[d_{n} = \max\left( \lvert (\mathbf{x}_{R} - \mathbf{x}_{L})\cdot\hat{\mathbf{n}} \rvert,\ 0.3\,\lvert \mathbf{x}_{R} - \mathbf{x}_{L}\rvert \right)\f] | | (9-3) | |

The floor keeps a near-degenerate sliver — where the centroid chord is
nearly parallel to the shared edge — from producing an unbounded
surface slope. Face roughness is the mean of the two cells' \f$n\f$.

Each cell also carries a characteristic length derived from the discrete
operator rather than from geometry, for the time-step bound of §9.5.5:

| | | | |
|---|---|---|---|
| \f[L_{char} = \sqrt{\frac{2A}{\sum_{f} \xi_{f}/d_{n,f}}}\f] | | (9-4) | |

A cell none of whose edges is an interior face has an empty sum in
(9-4) and keeps the altitude proxy \f$2A/\xi_{max}\f$ instead; such a cell
carries no interior flux until a neighbouring face opens, so the proxy
never governs a coupled update.

**Every quantity above is planimetric.** Areas, lengths and normals are
computed in plan rather than on the sloped surface. On terrain steep enough for
that distinction to matter the shallow-water equations are themselves
the wrong model, so this is a consistent rather than an incidental
choice.

Per-edge conveyance factors in \f$[0,1]\f$ may be attached with
`[2D_EDGE_CONVEYANCE]`, multiplying the flux across that edge — the
transmissivity \f$\psi\f$ of the integral-porosity shallow-water literature
(Sanders et al., 2008; Bruwier et al., 2017), used to represent
sub-grid obstructions such as walls and fences without meshing them.
Values are mirrored onto both slots of an interior edge, so the
restriction stays symmetric and the flux stays conservative.

Initial conditions come from `INIT_DEPTH` in `[2D_TRIANGLES]` (a depth
in mesh units, converted to a cell volume through the closure of §9.4)
and optionally `[2D_INITIAL_VELOCITY]`, which seeds face momentum from
the cell velocities. Without the latter a model can only start from
rest, which excludes solutions such as Thacker's oscillating basins
whose initial state has \f$\mathbf{u} \neq 0\f$.

## 9.4 Volume–free-surface closure

The conserved variable is cell **volume** \f$V\f$ rather than depth. Depth follows
as the cell-mean \f$\bar{h} = V/A\f$. The closure is the relation that
recovers a free-surface elevation \f$\eta\f$ from that volume, and it is
where a triangular mesh with sloping cell beds differs sharply from a
raster of flat cells.

`CELL_CLOSURE FLAT` (the default) uses

| | | | |
|---|---|---|---|
| \f[\eta = z_{c} + \bar{h}\f] | | (9-5) | |

which is exact for a fully wetted cell and wrong for a partially wet
one. On a cell whose bed spans a slope or a step, spreading the water
uniformly over the whole cell raises the computed surface above the true
waterline — by up to two-thirds of the cell's relief. That error is a
head, and a head drives flux: the classic symptom is thin films creeping
*uphill* at a shoreline, and a lake at rest that is not a steady state
where it meets the bank.

`CELL_CLOSURE VFR` uses the exact stage–storage relation of the plane
bed through the cell's three vertex elevations — the volume/free-surface
relationships of Begnudelli and Sanders (2006, 2007). With sorted
elevations \f$z_1 \le z_2 \le z_3\f$ and \f$\bar{z} = (z_1+z_2+z_3)/3\f$:

| | | | |
|---|---|---|---|
| \f[\bar{h}(\eta) = \frac{(\eta - z_{1})^{3}}{3(z_{2}-z_{1})(z_{3}-z_{1})}\f] | \f$z_{1} < \eta \le z_{2}\f$ | (9-6a) | |
| \f[\bar{h}(\eta) = (\eta - \bar{z}) + \frac{(z_{3}-\eta)^{3}}{3(z_{3}-z_{1})(z_{3}-z_{2})}\f] | \f$z_{2} < \eta \le z_{3}\f$ | (9-6b) | |
| \f[\bar{h}(\eta) = \eta - \bar{z}\f] | \f$\eta \ge z_{3}\f$ | (9-6c) | |

The solver needs the inverse \f$\eta(\bar{h})\f$. On the lower branch it is
a closed-form cube root; on the upper branch a safeguarded Newton
iteration bracketed by \f$[z_2, z_3]\f$, which converges unconditionally
because \f$d\bar{h}/d\eta = A_{wet}/A > 0\f$ there. Note that (9-6c) is
identical to the flat closure (9-5), since \f$z_c = \bar{z}\f$ — the two
closures differ only on partially wet cells, which is precisely the
claim.

As a cell dries, \f$d\eta/dV = 1/A_{wet}\f$ diverges. `VFR_MIN_WET_FRAC`
(\f$\varepsilon\f$, default 0.01) bounds it by continuing the exact relation
below wetted fraction \f$\varepsilon\f$ along its tangent line, keeping the
closure \f$C^1\f$ and monotone with \f$d\eta/dV \le 1/(\varepsilon A)\f$. The
regularized forward and inverse relations are exact inverses of each
other for the same \f$\varepsilon\f$, so seeding a state from heads and
reading it back as volumes round-trips.

**VFR is correct and is not the default.** It restores the C-property at
shorelines and removes the uphill-creep artifact, but in doing so it
resolves a wetting and drying front that the flat closure freezes out —
measured at three to eight times more solver substeps. For deep urban
flooding, where partially wet cells are a thin fringe around a large
wetted area, the flat closure is both faster and adequate. For shallow
sheet flow on gentle slopes, where most cells are partially wet, VFR is
the right choice and the cost is the price of the answer. Pair it with
`FACE_RECONSTRUCTION VFR_FACE` (§9.5.2); the two halves address the same
artifact from the cell side and the face side.

### 9.4.1 The closure algorithm as implemented

The relations (9-6) are evaluated by a single header-only routine
shared by the serial marcher, the boundary path and the GPU kernels, so
every backend reconstructs the identical surface. Three auxiliary
quantities appear. The wetted-area fraction of the planar bed —
which is also \f$d\bar{h}/d\eta\f$ — with sorted elevations
\f$z_1 \le z_2 \le z_3\f$ and relief \f$R = z_3 - z_1\f$:

| | | | |
|---|---|---|---|
| \f[w(\eta) = \frac{(\eta - z_{1})^{2}}{(z_{2}-z_{1})\,R}\f] | \f$z_{1} < \eta \le z_{2}\f$ | (9-20a) | |
| \f[w(\eta) = 1 - \frac{(z_{3} - \eta)^{2}}{R\,(z_{3}-z_{2})}\f] | \f$z_{2} < \eta < z_{3}\f$ | (9-20b) | |

with \f$w = 0\f$ below \f$z_1\f$ and \f$w = 1\f$ above \f$z_3\f$. The stage at which
the wetted fraction equals the regularization floor \f$\varepsilon\f$:

| | | | |
|---|---|---|---|
| \f[\eta_{s} = z_{1} + \sqrt{\varepsilon\,(z_{2}-z_{1})\,R}\f] | \f$\varepsilon \le (z_{2}-z_{1})/R\f$ | (9-21a) | |
| \f[\eta_{s} = z_{3} - \sqrt{(1-\varepsilon)\,R\,(z_{3}-z_{2})}\f] | otherwise | (9-21b) | |

Below the switch depth \f$\bar{h}_s = \bar{h}(\eta_s)\f$ from (9-6) the
inverse continues along the tangent line,

| | | | |
|---|---|---|---|
| \f[\eta(\bar{h}) = \eta_{s} - \frac{\bar{h}_{s} - \bar{h}}{\varepsilon}\f] | \f$\bar{h} \le \bar{h}_{s}\f$ | (9-22) | |

and on the lower branch it is closed-form,

| | | | |
|---|---|---|---|
| \f[\eta = z_{1} + \left\lbrack 3\,\bar{h}\,(z_{2}-z_{1})\,R \right\rbrack^{1/3}\f] | \f$\bar{h} \le (z_{2}-z_{1})^{2}/(3R)\f$ | (9-23) | |

The inverse \f$\eta(V)\f$ is evaluated by the following case ladder, in
this order:

1. Compute \f$\bar{h} = \max(V, 0)/A\f$ and sort the three vertex
   elevations in place.
2. **Flat cell.** \f$R < 10^{-9}\f$ m: the flat closure (9-5) is exact —
   \f$\eta = \bar{z} + \max(\bar{h}, 0)\f$.
3. **Fully wet.** \f$\bar{h} \ge z_3 - \bar{z}\f$: \f$\eta = \bar{z} + \bar{h}\f$,
   again identical to (9-5). This case is tested before the
   \f$\varepsilon\f$-tail because it dominates in deep water and the two
   branches never overlap — the fully-wet threshold always exceeds
   \f$\bar{h}_s\f$.
4. **Regularized tail.** \f$\varepsilon > 0\f$ and \f$\bar{h} \le \bar{h}_s\f$:
   the tangent line (9-22). With \f$\varepsilon = 0\f$ — the exact
   relation, used by the rendering reconstruction — a dry cell returns
   \f$\eta = z_1\f$ instead.
5. **Lower branch.** \f$\bar{h} \le (z_2 - z_1)^2/(3R)\f$: the cube root
   (9-23). When \f$z_2 = z_1\f$ to rounding this bound is zero and the
   branch is never selected.
6. **Degenerate upper interval.** \f$z_3 - z_2 < 10^{-9}\f$ m:
   \f$\eta = \bar{z} + \bar{h}\f$, so a one-ulp sliver never reaches the
   divisions of the Newton branch.
7. **Upper branch.** Safeguarded Newton on the bracket \f$[z_2, z_3]\f$
   for (9-6b), starting from the flat-closure guess
   \f$\eta = \bar{z} + \bar{h}\f$, with derivative
   \f$d\bar{h}/d\eta = 1 - (z_3-\eta)^2/(R(z_3-z_2)) = A_{wet}/A > 0\f$.
   Any Newton step leaving the bracket is replaced by bisection;
   iteration stops at \f$\lvert\Delta\eta\rvert < 10^{-12}(1 + R)\f$, with
   a 64-iteration cap that is never reached in practice.

The forward relation \f$\bar{h}(\eta)\f$ applies the same regularization —
exact above \f$\eta_s\f$, the tangent below, floored at zero — so forward
and inverse are exact inverses for the same \f$\varepsilon\f$ and
head-seeded and volume-seeded states round-trip. A dry cell's head
under VFR is seeded at \f$\eta(0) = \eta_s - \bar{h}_s/\varepsilon \in (z_1, \eta_s)\f$, the value the closure itself returns at \f$V = 0\f$, so
that seeding the head back through the forward relation reproduces
exactly zero volume. Figure 9-2 (§9.5.9) sketches the wetting cases.

Implementation: the closure lives in `src/engine/2d/mesh/VfrClosure.hpp`
— @ref openswmm::twoD::vfrSort3, @ref openswmm::twoD::vfrWetFraction,
@ref openswmm::twoD::vfrStageAtWetFraction,
@ref openswmm::twoD::vfrMeanDepthFromEtaExact,
@ref openswmm::twoD::vfrMeanDepthFromEta,
@ref openswmm::twoD::vfrEtaFromMeanDepth and
@ref openswmm::twoD::vfrDryEta. The solver enters through
@ref openswmm::twoD::inertial::etaDepthScalar and
@ref openswmm::twoD::inertial::volumeFromEtaScalar in
`src/engine/2d/solver/InertialKernels.hpp`, whose `FLAT` branch is
(9-5); the flat-relief guard is `kVfrFlatRelief` (\f$10^{-9}\f$ m).

## 9.5 Numerical scheme

The discretization is a cell-centred finite-volume method with a
staggered face variable: cells hold volume, faces hold the unit-width
discharge \f$q\f$ normal to the face, positive from \f$L\f$ to \f$R\f$. Time
integration is explicit, with per-cell local time stepping.

### 9.5.1 The face momentum update

Each face integrates (9-2) along its own normal over its own step
\f$\Delta t_f\f$:

| | | | |
|---|---|---|---|
| \f[q^{n+1} = \frac{\hat{q} - g\,h_{f}\,\Delta t_{f}\,S}{1 + g\,\Delta t_{f}\,n_{f}^{2}\,\lvert\mathbf{q}_{f}\rvert / h_{f}^{7/3}}\f] | | (9-7) | |

with the free-surface slope

| | | | |
|---|---|---|---|
| \f[S = \frac{\eta_{R} - \eta_{L}}{d_{n}}\f] | | (9-8) | |

Three details in (9-7) carry weight.

**Friction is semi-implicit.** Writing the friction term with \f$q^{n+1}\f$
in the numerator and \f$\lvert\mathbf{q}\rvert\f$ from the previous state
puts it in the denominator, which is unconditionally stable: however
large the friction coefficient, the update can only shrink \f$q\f$ towards
zero, never overshoot through it. An explicit friction term would impose
a step limit that scales as \f$h^{7/3}/n^{2}\f$ — unusable on thin films,
which is where most of the cells are.

**The friction magnitude is taken from the flow vector rather than the
face-normal component.** Manning friction acting on the normal component is
\f$n^{2} q_{n} \lvert\mathbf{q}\rvert / h^{7/3}\f$. Using \f$\lvert q_n \rvert\f$ instead makes the damping a face applies depend on the face's
orientation relative to the flow — a face at 45° to a uniform sheet
under-damps by \f$\sqrt{2}\f$ — so no smooth surface can satisfy every face
of a triangulated slope simultaneously, and the steady state corrugates
cell to cell. The vector at the face is the mean of the two incident
cells' Perot-reconstructed discharge vectors (§9.5.4, Eq. 9-16), and
its magnitude is floored at the face's own discharge:

| | | | |
|---|---|---|---|
| \f[\lvert\mathbf{q}_{f}\rvert = \max\!\left( \lvert q_{f} \rvert,\ \left\lvert \tfrac{1}{2}(\mathbf{q}_{L} + \mathbf{q}_{R}) \right\rvert \right)\f] | | (9-24) | |

so a face whose cell reconstruction lags its own discharge — the first
firing after a front arrives, or immediately after activation — never
under-damps. With \f$\theta = 1\f$ the cell discharge vectors are not
allocated at all: the update uses \f$\hat{q} = q_f\f$ and
\f$\lvert\mathbf{q}_f\rvert = \lvert q_f \rvert\f$, recovering the original
Bates et al. (2010) scheme exactly. The friction exponent is evaluated
as \f$h^{7/3} = h^{2}\sqrt[3]{h}\f$ rather than through `pow()`.

**\f$\hat{q}\f$ is a lateral average, not \f$q\f$ itself.** With
`THETA` \f$= \theta\f$,

| | | | |
|---|---|---|---|
| \f[\hat{q} = \theta\,q_{f} + (1-\theta)\,\tfrac{1}{2}\left( \mathbf{q}_{L} + \mathbf{q}_{R} \right)\cdot\hat{\mathbf{n}}\f] | | (9-9) | |

\f$\theta = 1\f$ recovers the original Bates et al. (2010) scheme, which
carries no numerical diffusion and is prone to a checkerboard
oscillation in thin films on steep faces. \f$\theta < 1\f$ blends in the
neighbouring cells' reconstructed discharge and damps it — the weighted
formulation of de Almeida et al. (2012). The default \f$\theta = 0.8\f$
applies enough diffusion to suppress the oscillation without visibly
smearing fronts.

Finally the result is clamped:

| | | | |
|---|---|---|---|
| \f[\lvert q^{n+1} \rvert \le Fr_{max}\,h_{f}\sqrt{g\,h_{f}}\f] | | (9-10) | |

`FROUDE_MAX` defaults to 1.5. This is the steep-face guard: with no
convective term there is nothing in (9-2) to arrest acceleration down a
steep face, so the supercritical limit must be imposed rather than
resolved. Raising it lets genuinely transcritical cases run (§9.10) at
the cost of the guard.

As coded, one face firing over its step \f$\Delta t_f\f$ proceeds in a
fixed order: (i) evaluate the face depth (§9.5.2) and wall the face if
\f$h_f \le\f$ `DRY_DEPTH`, zeroing its momentum; (ii) form \f$\hat{q}\f$ by
(9-9) and the friction magnitude by (9-24); (iii) zero the free-surface
difference if it lies below the \f$10^{-12}\f$ m deadband (§9.5.3) and
form the slope (9-8); (iv) apply (9-7) and then the clamp (9-10);
(v) rescale by the positivity share (9-15) where it binds; (vi) book
\f$\pm\Delta M\f$ by (9-13). The stored face discharge is the post-clamp,
post-rescale value, so the momentum a face carries always matches the
mass it moved.

### 9.5.2 Face flow depth and wetting/drying

The depth in (9-7) is a property of the face, and how it is defined
decides when water is allowed to cross. Under
`FACE_RECONSTRUCTION MEAN` (the default):

| | | | |
|---|---|---|---|
| \f[h_{f} = \max(\eta_{L}, \eta_{R}) - \max(z_{c,L}, z_{c,R})\f] | | (9-11) | |

\f$h_f \le\f$ `DRY_DEPTH` makes the face a wall for that substep and its
momentum is zeroed. This is the standard "flow depth above the higher
bed" rule, and it is what makes the scheme handle wetting and drying
without regime-switching logic — but its bed is the higher *centroid*
elevation. A thin crest resolved as a line of high vertices — a levee, a
kerb, a road crown — has its height diluted by roughly a third when
averaged into the flanking centroids, so water crosses it before it
reaches it.

`FACE_RECONSTRUCTION VFR_FACE` uses instead the exact mean depth of the
driving surface over the wetted portion of the shared edge, evaluated
against the edge's **true endpoint elevations** \f$z_{lo} \le z_{hi}\f$
(Begnudelli and Sanders, 2007, Eq. 14):

| | | | |
|---|---|---|---|
| \f[h_{f} = 0\f] | \f$\eta \le z_{lo}\f$ | (9-12a) | |
| \f[h_{f} = \frac{(\eta - z_{lo})^{2}}{2(z_{hi}-z_{lo})}\f] | \f$z_{lo} < \eta \le z_{hi}\f$ | (9-12b) | |
| \f[h_{f} = \eta - \tfrac{1}{2}(z_{lo}+z_{hi})\f] | \f$\eta > z_{hi}\f$ | (9-12c) | |

with \f$\eta = \max(\eta_L, \eta_R)\f$. The quadratic branch matches value
and slope at both joins, so overtopping onset is \f$C^1\f$ and the flux does
not jump when the waterline crosses the edge. The gate (9-12a) is the
substantive part: a cell holding water pooled below the whole shared
edge conveys nothing across it. Embankments hold to their real crest,
and drainage no longer strands water on slopes.

Both branches are single-sourced and used identically by the interior
faces, the boundary edges and the GPU kernels, so all backends agree.

### 9.5.3 Well-balancedness

A body of water at rest over arbitrary bathymetry must stay at rest.
This is the C-property, and it is not automatic — a scheme that
discretizes the bed slope and the pressure gradient separately will
generally produce a spurious flux from their imbalance.

Writing the pressure gradient as the free-surface slope (9-8) makes the
property structural: at rest \f$\eta_L = \eta_R\f$, so \f$S = 0\f$ exactly, and
(9-7) with \f$\hat{q} = q = 0\f$ returns zero for any bed whatsoever.
Likewise a dry neighbour standing higher gives \f$h_f \le 0\f$ and the face
is a wall — there is no uphill creep to suppress.

One numerical guard is needed. The closure round-trip \f$V \to \eta\f$
introduces rounding noise of order 1 ulp, and the square-root character
of the friction balance amplifies it: a persistent \f$\Delta\eta \sim 10^{-16}\f$ m sustains \f$q \sim 10^{-6}\f$ m²/s. A slope below
\f$10^{-12}\f$ m is therefore set to exactly zero, far below any physical
head, after which the friction denominator decays \f$q\f$ geometrically and
rest states are exact rather than merely small. §9.10 measures the
result at \f$10^{-16}\f$ relative error on the SWASHES lake-at-rest cases.

### 9.5.4 The cell update, conservation and positivity

A face firing books the identical volume transfer into a per-side
accumulator:

| | | | |
|---|---|---|---|
| \f[\Delta M = q^{n+1}\,\xi\,\Delta t_{f}, \qquad \text{acc}_{L} \mathrel{-}= \Delta M, \quad \text{acc}_{R} \mathrel{+}= \Delta M\f] | | (9-13) | |

and a cell firing gathers and clears its own side of each incident
accumulator:

| | | | |
|---|---|---|---|
| \f[V^{n+1} = V^{n} + \sum_{f} \text{acc}_{f,i} + \Delta t_{c}\,A\,(i - e + s)\f] | | (9-14) | |

after which \f$\eta\f$ and \f$\bar{h}\f$ are recomputed through the closure of
§9.4. Because the two sides of a face are written from the *same*
floating-point product, conservation is exact by construction rather
than to within a tolerance — including across a local-time-stepping tier
interface, where the two sides apply their halves at different times
(§9.5.6). The sum of cell volumes plus pending accumulators is an
invariant of the face phase, and the engine can assert it directly
(`OPENSWMM_2D_MARCHER_CHECK`).

Positivity is enforced at face cadence rather than by a post-hoc clamp.
A cell has at most three outgoing faces, so capping each exporting face
at a share \f$\beta/3\f$ of its exporting cell's volume bounds the total
export at \f$\beta V\f$ per cell step without any cross-face coordination:

| | | | |
|---|---|---|---|
| \f[\lvert q^{n+1}\rvert\,\xi\,\Delta t_{f} \le \frac{\beta}{3}\,\frac{V_{exp}}{2^{\,k_{exp} - k_{f}}}\f] | | (9-15) | |

\f$\beta\f$ is `exchange_beta` (0.8). The tier ratio in the denominator
matters: an exporting cell republishes its volume only at its own
firings, and a finer face fires \f$2^{k_{exp}-k_f}\f$ times in between, so
without dividing the share the repeated takes drain the cell. When a
face is rescaled, the *same* rescaled flux updates both sides, so the
cap costs nothing in conservation. A zero floor at the cell update
remains as a backstop; with the face caps in place it does not engage.

The cell's discharge vector — needed for the friction magnitude and the
\f$\theta\f$ blend — is reconstructed at the cell's own cadence from its
face fluxes by the Perot (2000) formula:

| | | | |
|---|---|---|---|
| \f[\mathbf{q}_{i} = \frac{1}{A_{i}}\sum_{f} s_{f}\,q_{f}\,\xi_{f}\,\left( \mathbf{x}_{f} - \mathbf{x}_{i} \right)\f] | | (9-16) | |

with \f$s_f = \pm 1\f$ the outward orientation of face \f$f\f$ for cell \f$i\f$ and
\f$\mathbf{x}_f\f$ the edge midpoint.

### 9.5.5 The time step

Each cell's stable step follows from the gravity-wave celerity and its
own characteristic length:

| | | | |
|---|---|---|---|
| \f[\Delta t_{i} = \alpha \frac{L_{char,i}}{\sqrt{g h_{i}} + \lvert \mathbf{u}_{i}\rvert}\f] | | (9-17) | |

with \f$\alpha =\f$ `CFL_NUMBER`, default 0.7, and the base step of a macro
cycle \f$\Delta t_0 = \min_i \Delta t_i\f$, further capped by
`MAX_TIMESTEP`. Only active cells wetter than `DRY_DEPTH` enter the
census — a film the solver will not move imposes no constraint — and a
fully quiescent active set falls back to \f$\Delta t_0 =\f$ `MAX_TIMESTEP`.
The advective augmentation \f$\lvert\mathbf{u}_i\rvert = \lvert\mathbf{q}_i\rvert/h_i\f$ is evaluated from the Perot vector when
the depth exceeds \f$10^{-6}\f$ m and \f$\theta < 1\f$, and taken as zero
otherwise.

\f$L_{char}\f$ is the operator-derived length of (9-4) rather than a
geometric proxy, and the difference matters. The face update couples cells through \f$g h \xi_f/(A\,d_{n,f})\f$;
the worst (odd–even) mode of that operator has eigenvalue \f$\lambda = 2(gh/A)\sum_f \xi_f/d_{n,f}\f$, and the explicit update is linearly stable
for \f$\Delta t \le 2/\sqrt{\lambda}\f$, which is exactly (9-4) divided by
the celerity. Defining \f$L_{char}\f$ this way makes \f$\alpha\f$ a **true
Courant fraction**: \f$\alpha = 1\f$ is the linear stability limit on any
mesh, and the default 0.7 is a uniform 30 % margin. A raster of squares
recovers the classical \f$c\,\Delta t/\Delta x \le 1/\sqrt{2}\f$; a
union-jack pair of right triangles gets \f$0.408\,\Delta x\f$. The obvious
geometric proxy \f$2A/\xi_{max}\f$ returns \f$0.707\,\Delta x\f$ on that same
union-jack mesh — an overstatement by \f$\sqrt{3}\f$, and the reason
frictionless basins seiched at nominal Courant numbers that looked
conservative.

Between full rebuilds the tier lists are frozen while depths keep
evolving, so \f$\Delta t_0\f$ is re-minimized every macro cycle. It may be
**tightened** at any time — every tier still satisfies \f$\Delta t_i \ge 2^{k}\Delta t_0\f$ — but growing it requires reassigning tiers and
therefore waits for a rebuild.

### 9.5.6 Local time stepping

A flood mesh is heterogeneous by nature: a 0.5 m cell at a coupled
manhole and a 20 m cell on a floodplain differ by two orders of
magnitude in stable step. Marching the whole mesh at the smallest one
wastes almost all of the work.

The solver instead assigns each cell a power-of-two tier \f$k\f$ from the
ratio \f$\Delta t_i/\Delta t_0\f$, capped at `LTS_TIERS` (default 4,
allowing an 8× spread; up to 8 tiers, 128×). A macro cycle is
\f$2^{K-1}\f$ base substeps; tier \f$k\f$ fires every \f$2^{k}\f$ substeps with
\f$\Delta t = 2^{k}\Delta t_0\f$. Within a substep all due faces fire first,
then all due cells — faces read the surfaces their incident cells
published at those cells' last firings.

**A face belongs to the finer of its two incident cells' tiers.** It
therefore always integrates at the rate the sharper side needs, reading
the coarser side's surface frozen since that cell last fired. This is
what makes tier interfaces safe without interpolation, and (9-13) is
what makes them conservative: the same \f$\pm\Delta M\f$ is booked once and
applied by each side at its own firing.

Cells whose forcing changes at the fastest cadence are pinned to tier 0
regardless of their Courant number — boundary cells and cells carrying a
1D coupling point. For these cells the forcing, rather than the
celerity, sets the resolution requirement.

### 9.5.7 The active set

Most of a rain-on-grid mesh is not flowing. A cell is **flux-active**
only above `H_MOVE` (default 3 mm), with hysteresis: entering cells need
\f$h_{move} + \delta\f$, active cells stay until \f$h_{move} - \delta\f$, where
\f$\delta = \min(1\ \text{mm},\ h_{move}/2)\f$. Scaling the band with
`H_MOVE` matters on shallow benchmarks — a fixed ±1 mm band made
`H_MOVE` \f$= 10^{-4}\f$ require 1.1 mm to activate, ten times the requested
threshold, which freezes wetting fronts in place.

Two rules complete the set. **A face flows only when both incident cells
are active** — a one-sided face would export volume into a cell whose
update never runs, and measured as an 18 % basin loss when it was
allowed. A **one-ring halo** around the active set therefore guarantees
an advancing front always has an active receiving cell.

Inactive cells are not skipped, they are integrated **lazily**: rainfall
and held coupling accumulate as pure storage over the whole interval
since the last synchronization, in one pass, because a cell below
`H_MOVE` has no face flux by construction. This is what makes
rain-on-grid over a large dry mesh nearly free. A rainfall rate as such
never activates a cell — activation follows only from the accumulated
depth crossing the threshold at a rebuild, which is the point of the
lazy tier — whereas a nonzero coupling flux activates its cell
immediately, as do the pinned boundary and coupling-point cells of
§9.5.6.

The active set and the tier assignment are rebuilt every four macro
cycles rather than every substep; the cost of the rebuild is \f$O(n_{cells})\f$
and dominated everything else when it ran per routing step on a large
mesh.

### 9.5.8 Data layout and the substep algorithm

The solver's working set is three structure-of-arrays blocks, sized
once at initialization.

**Per unique interior face** (built by the edge enumeration of §9.3):
the incident cells \f$c_L < c_R\f$; the edge length \f$\xi\f$; the unit normal
\f$\hat{\mathbf{n}}\f$ oriented \f$L \to R\f$ and the edge midpoint; the
reciprocal face-normal centroid separation \f$1/d_n\f$ of (9-3); the
squared face roughness \f$n_f^2 = \left(\tfrac{1}{2}(n_L + n_R)\right)^2\f$; the sorted true endpoint bed elevations \f$z_{lo} \le z_{hi}\f$ of the shared edge (for `VFR_FACE`); and the two flat
edge-slot indices \f$3t + e\f$ used to publish fluxes back into the
per-cell edge arrays. The prognostic state per face is the discharge
\f$q\f$ and the two pending-transfer accumulators \f$\text{acc}_L\f$,
\f$\text{acc}_R\f$ of (9-13), plus a face tier.

**Per cell**: the conserved volume \f$V\f$ and the reconstructed \f$\eta\f$ and
\f$\bar{h}\f$; the source rates (rainfall, evaporation demand, coupling
flux, all m/s); the Perot discharge vector \f$(q_{cx}, q_{cy})\f$,
allocated only when \f$\theta < 1\f$; the characteristic length \f$L_{char}\f$
of (9-4); the tier, the active flag and the tier-0 pin flag; and a CSR
incidence — for cell \f$i\f$ the incident unique faces with orientation
signs \f$\pm 1\f$ — so the continuity gather is a race-free per-cell loop.

**Per vertex**: coordinates and bed elevation, plus the reconstruction
stencils of §9.9. Per flat edge slot: the published volumetric flux
and the boundary-condition tables of §9.6.

One solver advance over a window \f$[t,\ t + \Delta t]\f$ executes:

1. Reset the per-advance ledgers: boundary accumulators, coupling
   accumulators \f$\int Q\,dt\f$, and the per-node spill budget.
2. Loop until the window is filled:
   1. Every fourth macro cycle, **rebuild**: settle all pending face
      accumulators into their cells; integrate lazy sources on
      inactive cells over the interval since the last synchronization;
      reseed the active set with the hysteretic threshold of §9.5.7
      plus the pinned cells; grow the one-ring halo; recompute the CFL
      census and \f$\Delta t_0\f$; assign cell tiers
      \f$k = \min\!\left(K - 1, \lfloor \log_2(\Delta t_i/\Delta t_0) \rfloor\right)\f$ with pinned and coupled cells forced to \f$k = 0\f$;
      set each face's tier to the finer of its cells and zero the
      momentum of any face with an inactive side. Between rebuilds,
      only re-minimize \f$\Delta t_0\f$ from the live depths — tightening
      is always safe, growing waits for the rebuild.
   2. If the active set is empty, stride to the end of the window; the
      lazy tier keeps accumulating.
   3. Set \f$\Delta t_0 \leftarrow \min(\Delta t_0, \text{remaining})\f$.
      If a full macro cycle of \f$2^{K-1}\f$ base substeps would overshoot
      the window, settle the accumulators, collapse every active cell
      to tier 0, and finish the window with single global substeps
      (the *tail*); a rebuild is forced afterwards.
   4. Run the macro cycle: for each base substep \f$s\f$, fire the faces
      of every due tier (\f$s \bmod 2^k = 0\f$) with \f$\Delta t_f = 2^k\Delta t_0\f$ in the order of §9.5.1, then fire the due cells
      with \f$\Delta t_c = 2^k \Delta t_0\f$ — each cell gathers and
      clears its own side of every incident accumulator, applies its
      sources, floors the volume at zero, reruns the closure of §9.4
      and refreshes its Perot vector (9-16). Tier-0 cell firings
      additionally integrate the boundary edges of §9.6 and the live
      junction exchange of §9.7.
3. Land any remaining lazy sources at the window end.
4. Publish the flux picture: interior faces re-limit \f$q\f$ against the
   published surfaces (the update's own clamp used the depths it saw;
   the subsequent cell pass moved them) and write \f$\pm q\,\xi\f$ into
   both edge slots; boundary slots carry the window-mean applied flux
   so the router's booking recovers the exact applied volume.

The face and cell passes are OpenMP-parallel with static scheduling
and the project's `THREADS` setting. Each face is written by exactly
one iteration and touches only its own accumulator slots, and each
cell gathers only its own accumulator sides, so both passes are
race-free and bit-identical to serial execution for any thread count.
The boundary and coupling loops are serial; they are perimeter- and
point-count-sized. The quantity \f$\sum_i V_i + \sum_f (\text{acc}_{L,f} + \text{acc}_{R,f})\f$ is invariant under the face phase and is asserted
directly when `OPENSWMM_2D_MARCHER_CHECK` is set.

Figure 9-1 assembles the co-advance batch of §9.7.3 and the marcher's
substep loop into one workflow.

<pre class="mermaid">
flowchart TD
    A[1D routing step completes - node heads current] --> B{Pending span reaches the sync batch}
    B -- no --> A
    B -- yes --> C[Save state and seed withdrawal budgets]
    C --> D[Accumulate outfall discharge and inject as batch-rate source]
    D --> E[Refresh rainfall, forcing overrides and boundary values]
    E --> F[Marcher advance over the batch span]
    F --> G{Rebuild due}
    G -- yes --> H[Settle accumulators, lazy sources, active set, tiers, dt0]
    G -- no --> I[Tighten dt0 from live depths]
    H --> J[Macro cycle of base substeps]
    I --> J
    J --> K[Fire due faces - depth, blend, update, clamp, positivity, book dM]
    K --> L[Fire due cells - gather, sources, closure, Perot]
    L --> M[Tier-0 firings - boundary edges and live junction exchange]
    M --> N{Batch span filled}
    N -- no --> G
    N -- yes --> O[Book junction, outfall and boundary ledgers into the 2D mass balance]
    O --> P[Queue exchange volumes for uniform delivery to 1D lateral inflow]
    P --> Q[Clear one-shot forcings, reset window accumulators]
    Q --> A
</pre>

*Figure 9-1 One 1D–2D co-advance batch and the explicit marcher's
substep loop within it (rendered diagram)*

Implementation:
@ref openswmm::twoD::ExplicitInertialSolver::advance drives the loop;
@ref openswmm::twoD::ExplicitInertialSolver::fireFaces,
@ref openswmm::twoD::ExplicitInertialSolver::fireCells,
@ref openswmm::twoD::ExplicitInertialSolver::syncAndRebuild,
@ref openswmm::twoD::ExplicitInertialSolver::settleAccumulators and
@ref openswmm::twoD::ExplicitInertialSolver::refreshDt0 implement the
numbered steps. The face layout and CSR incidence are built by
@ref openswmm::twoD::InertialEdges::build
(`src/engine/2d/solver/InertialEdges.cpp`); the scalar kernels live in
`src/engine/2d/solver/InertialKernels.hpp`; the cell state arrays are
@ref openswmm::twoD::SurfaceStateData
(`src/engine/2d/data/SurfaceStateData.hpp`).

### 9.5.9 Wetting and drying: the complete rule set

The scheme has no regime-switching logic; wetting and drying emerge
from a small set of thresholds applied uniformly. Table 9-1 collects
them.

| Constant | Value | Origin | Role |
|---|---|---|---|
| `DRY_DEPTH` | 0.001 m | `[2D_OPTIONS]` | Face-wall depth, friction depth floor, evaporation taper scale, coupling ramp scale, CFL census cutoff |
| `H_MOVE` | 0.003 m | `[2D_OPTIONS]` | Flux-activation depth |
| \f$\delta\f$ | \f$\min(0.001\ \text{m},\ h_{move}/2)\f$ | derived | Activation hysteresis half-band |
| \f$\varepsilon\f$ | 0.01 | `VFR_MIN_WET_FRAC` | Wetted-fraction floor of the VFR closure |
| slope deadband | \f$10^{-12}\f$ m | fixed | Free-surface differences treated as exactly zero |
| flat-relief guard | \f$10^{-9}\f$ m | fixed | Cell or edge relief below which the geometry is flat |
| \f$\beta\f$ | 0.8 | fixed | Positivity and exchange availability fraction |
| rebuild cadence | 4 macro cycles | fixed | Active-set and tier refresh period |

*Table 9-1 Wetting and drying thresholds and guards of the 2D solver
(SI units)*

**The face-wet criterion.** A face conveys only when its flow depth
exceeds `DRY_DEPTH`: under `MEAN` the depth (9-11), under `VFR_FACE`
the wetted-edge depth (9-12) of the driving surface over the edge's
true endpoint beds. A face that fails the test is a wall for that
substep and its momentum is set to exactly zero — walls carry no stale
discharge into their next wet substep. A face also requires both
incident cells active (§9.5.7); faces bordering an inactive cell have
their momentum zeroed at the rebuild.

**How a dry cell wets.** The activation thresholds carry hysteresis,

| | | | |
|---|---|---|---|
| \f[h_{on} = h_{move} + \delta, \qquad h_{off} = \max(0,\ h_{move} - \delta), \qquad \delta = \min(0.001,\ h_{move}/2)\f] | | (9-25) | |

entering cells needing \f$h_{on}\f$ and active cells persisting to
\f$h_{off}\f$. A dry cell gains water in one of three ways. Distributed
sources (rainfall) accumulate lazily as pure storage; the cell joins
the active set at the first rebuild whose census finds its depth at or
above \f$h_{on}\f$. A concentrated source — a nonzero coupling flux —
activates the cell at the next rebuild regardless of depth, as does
membership in the pinned set. A neighbouring active cell activates it
through the one-ring halo, after which the shared face joins the face
lists; the first term to act on the newly wet cell is then the mass
transfer (9-13), driven by the slope term of (9-7) integrated from
\f$q = 0\f$ — the friction denominator is near unity at \f$q = 0\f$, so the
initial specific discharge after one face step is \f$-g\,h_f\,\Delta t_f\,S\f$.

**How a front advances.** Between rebuilds the active set is frozen,
so a wetting front can cross at most one cell ring per rebuild period
(four macro cycles). This is not a practical restriction: a fast front
implies a small \f$\Delta t_0\f$ through (9-17), so the rebuild period
shrinks with the front's own time scale, and the halo guarantees the
front always finds an active receiving cell — a one-sided face is
never allowed to fire (§9.5.7).

**Shorelines and the lake at rest.** A dry neighbour standing higher
gives \f$h_f \le 0\f$ under (9-11) and the face is a wall — there is no
uphill creep to suppress. Under the VFR closure the partially wet
shoreline cell's \f$\eta\f$ is exact rather than biased high (§9.4), and
under `VFR_FACE` the gate (9-12a) blocks conveyance across any edge
whose low point stands above the driving surface, so an emerged bump
holds a lake at rest exactly. The \f$10^{-12}\f$ m slope deadband removes
the closure round-trip noise that would otherwise sustain a
\f$\sim 10^{-6}\f$ m²/s residual discharge (§9.5.3); the emerged-bump
verification case of §9.10 measures the combined result at
\f$1.4 \times 10^{-16}\f$ relative depth error.

**Draining and positivity.** Every exporting face is capped at the
tier-scaled \f$\beta/3\f$ share of its exporting cell's volume, Eq.
(9-15), so a cell's at most three outgoing faces can remove at most
\f$\beta V\f$ per cell step with no cross-face coordination. Boundary
edges and the coupling exchange clamp in volume space — the applied
change is recomputed after flooring the provisional volume at zero, so
booking matches application exactly. The cell update keeps a plain
zero floor as a backstop; with the caps in place it does not engage
(a debug build with `OPENSWMM_2D_MARCHER_CHECK` reports any clamp
below \f$-10^{-12}\f$ m³).

**The friction depth floor.** No separate floor exists: the face-wall
test guarantees \f$h_f >\f$ `DRY_DEPTH` \f$= 1\f$ mm before (9-7) is
evaluated, so \f$h_f^{7/3} \ge 10^{-7}\f$ m\f$^{7/3}\f$ and the semi-implicit
denominator is always finite. Because friction enters only through the
denominator, it can shrink \f$q\f$ toward zero and never reverse it.

**Rain and evaporation on dry cells.** The evaporation sink is the
demand rate tapered by a cubic Hermite ramp below `DRY_DEPTH`,

| | | | |
|---|---|---|---|
| \f[e_{eff} = e \cdot \sigma\!\left( h/h_{dry} \right), \qquad \sigma(t) = \min(1, t)^{2}\left( 3 - 2\min(1, t) \right)\f] | | (9-26) | |

with \f$e_{eff} = 0\f$ for \f$h \le 0\f$ or \f$e \le 0\f$ — a drying cell cannot
evaporate more water than it holds, and negative demand is treated as
zero rather than as a condensation source. Rainfall on an inactive
cell integrates in a single lazy pass over the whole interval since
the last synchronization; the result is floored at zero volume, so a
forced evaporation override on a dry cell is harmless.

Figure 9-2 sketches the geometry the rules act on: the three wetting
cases of the planar-bed cell (§9.4.1) and the wetted-edge face gate
(§9.5.2).

<!-- Intended final drawing for Figure 9-2: a perspective or plan-view
     schematic of one triangular cell with a sloping (planar) bed at
     three stages — waterline below z2 (wetted subtriangle at the low
     vertex), waterline between z2 and z3 (dry corner at the high
     vertex), and fully submerged — each annotated with z1, z2, z3, η
     and the wetted region; plus an edge-profile inset showing the
     shared-edge endpoint beds z_lo, z_hi and the three branches of the
     face-depth relation (blocked, partially submerged, fully
     submerged). -->
![Figure 9-2](figure9-2-placeholder.png)

*Figure 9-2 Wetting cases of a planar-bed triangular cell and the
wetted-edge face gate (placeholder — to be replaced by a final
drawing)*

Implementation: the face-wet tests are
@ref openswmm::twoD::inertial::faceFlowDepth and
@ref openswmm::twoD::inertial::faceFlowDepthVfr over
@ref openswmm::twoD::inertial::faceDepthFromEta; the positivity share
is applied in @ref openswmm::twoD::ExplicitInertialSolver::fireFaces
with the scale factor of
@ref openswmm::twoD::inertial::positivityScale; the hysteretic
activation, halo and lazy source passes are
@ref openswmm::twoD::ExplicitInertialSolver::syncAndRebuild and
@ref openswmm::twoD::ExplicitInertialSolver::lazySourcesOnly; the
evaporation taper is @ref openswmm::twoD::evapSink
(`src/engine/2d/solver/SurfaceFluxCalculator.hpp`). The slope deadband
is `kEtaDeadband` in `src/engine/2d/solver/InertialKernels.hpp`.

## 9.6 Boundary conditions

Boundary edges — those claimed by only one triangle — default to
no-flux walls. `[2D_BOUNDARY_CONDITIONS]` assigns any of five types per
edge:

| Type | Parameter | Meaning |
|---|---|---|
| `WALL` | — | Zero flux (default) |
| `NORMAL_FLOW` | bed slope \f$S\f$ | Manning outflow \f$q = h^{5/3}\sqrt{S}/n\f$ per metre of edge |
| `SPECIFIED_STAGE` / `TS_STAGE` | head, or time series | Prescribed free-surface elevation |
| `SPECIFIED_FLOW` / `TS_FLOW` | discharge per metre, or time series | Prescribed unit discharge, outward positive |
| `RATING_CURVE` | curve name | Stage → unit discharge lookup, resolved each step from the boundary cell's stage |

Time series and curve names are resolved to registry indices once, on
the first advance, and evaluated every routing step thereafter.
Prescribed stages share the mesh's vertical datum and prescribed flows
the project's flow units, so both are converted to SI on the same terms
as the mesh itself (§9.7.4).

**A stage boundary is integrated with the interior momentum law**, not
with a conductance. The ghost state holds \f$\eta = \eta_{bc}\f$ with a
zero-gradient discharge, sitting across the edge at the centroid-to-edge
distance \f$2A/(3L)\f$, and (9-7) is applied to it exactly as to an interior
face. The earlier treatment — a collapsed Manning flux toward the
prescribed stage — was a diffusive-wave law grafted onto an inertial
interior, and it showed: its conductance saturated the equilibrium
clamp into a Dirichlet cell, and every boundary-driven steady case
floated one head jump, of order \f$v^{2}/2g\f$, above the stage it had been
given. The bump cases of §9.10 pass because of this change.

A per-substep equilibrium clamp remains as a backstop: one substep may
move a cell at most to the prescribed stage, never past it. At the
inertial law's gravity-scale fluxes it rarely binds; on a tiny cell it
prevents overshoot. Exchange is clamped in **volume** space and the
booked flux re-derived from the applied change, so what is reported is
exactly what was applied.

Cumulative boundary volume is tracked per edge, outward positive, and
enters the 2D mass balance of §9.9.

## 9.7 Coupling to the one-dimensional network

Coupling is bidirectional and mass-conservative, and it is where a 2D
module earns or loses its credibility. Two mechanisms exist, one for
junctions and one for outfalls.

### 9.7.1 Junction exchange

`[2D_VERTEX_NODE_MAP]` and `[2D_TRIANGLE_NODE_MAP]` associate a mesh
vertex or cell with a SWMM node, with a discharge coefficient \f$C_d\f$
(default 0.65) and an exchange area. Exchange is an orifice law on the
head difference:

| | | | |
|---|---|---|---|
| \f[Q = C_{d}\,A_{eff}\,\mathrm{sign}(\Delta h)\,\sqrt{2g}\ \varphi\!\left(\lvert\Delta h\rvert\right), \qquad \Delta h = h_{2D} - h_{1D}\f] | | (9-18) | |

positive draining the surface into the network. Three regularizations
turn (9-18) from a stiffness source into something an explicit solver
can integrate:

**A bounded square root.** \f$dQ/d\Delta h \to \infty\f$ as \f$\Delta h \to 0\f$
is exactly the regime a fill-and-spill manhole hovers in. Below 2 cm,
\f$\varphi\f$ is a \f$C^1\f$ quadratic matching \f$\sqrt{x}\f$ in value and slope at
the join and having finite slope at zero.

**A capped-pipe gate.** A manhole with its lid on exchanges through the
network only when the higher of the two heads reaches the crown
elevation \f$z_{inv} + D_{full}\f$. A Hermite smoothstep over a 5 cm band
above the crown opens the exchange, and the effective area transitions
smoothly from the inlet area to twice it over the same scale as the node
surcharges.

**Source-side wet/dry ramps.** \f$Q\f$ is multiplied by a smoothstep on the
*source* side's depth relative to `DRY_DEPTH`, so a drain self-limits to
zero as the cell empties and a spill self-limits as the node empties.
This replaces a held-flux availability cap and is what makes the
exchange stable inside the solver's inner loop rather than only across
a window.

The exchange is evaluated **live, at tier-0 cadence**, against the
current 2D heads and the routing step's 1D heads, and \f$\int Q\,dt\f$ is
accumulated exactly per point. Two hard caps make the ledger
unfalsifiable: a drain may take at most \f$\beta\f$ of the source cell's
volume per substep, and a spill draws against a per-node budget of the
node's stored volume for the whole advance — so the same water cannot
spill twice within a routing step.

The 2D head at a coupling point is not simply a cell head. Vertex-coupled
points use a **wet-masked, depth-weighted mean** of the incident cells'
free surfaces under the VFR closure: a manhole vertex is commonly
carved below the surrounding terrain, and a geometric average over
incident cells would read dry-cell bed elevations as a water surface and
pin the exchange at a phantom head from the first step.

Under dynamic wave routing the exchange would otherwise be a
zero-sensitivity explicit source in the node continuity equation, which
churns the Picard iteration. The head sensitivity

| | | | |
|---|---|---|---|
| \f[G = -\frac{\partial Q}{\partial h_{1D}} = C_{d}A_{eff}\sqrt{2g}\ \varphi'\!\left(\lvert\Delta h\rvert\right)\cdot(\text{gate})\cdot(\text{ramp}) \ \ge\ 0\f] | | (9-19) | |

is scattered into the node's \f$\sum dQ/dH\f$ denominator each iteration.
The gate and ramp derivatives are deliberately dropped so the term can
only be positive — a pure damping contribution, never a destabilizing
one.

Exchange volumes reach the 1D side through the lateral-inflow **delivery
queue**, drained at a uniform rate over the batch span rather than as a
single-step pulse.

### 9.7.2 Outfalls

An outfall coupled to the mesh works in both directions.

Outward, the node's net discharge for the routing step is accumulated
and injected into the 2D cells as a constant-rate source over the
subcycle. Withdrawals — a submerged outfall drawing surface water back
into the pipe — are capped by a per-cell budget seeded from the state
the batch started from, so a batch's cumulative withdrawal can never
overdraw it.

Inward, the 2D surface acts as **dynamic tailwater**. The 2D stage at
the coupling point is cached, and the outfall's boundary condition
becomes \f$\max(h_{standard}, h_{2D})\f$, applied inside the dynamic-wave
iteration so it survives every Picard pass. Flap gates are honoured:
the gate decision is made where the current \f$h_{standard}\f$ is visible.

The wet/dry gate here is keyed on depth *in excess of* `DRY_DEPTH`,
not on depth. The reason is specific: a draining cell comes to rest at a
film at or just below `DRY_DEPTH`, which the solver treats as immovable.
A ramp keyed on depth alone would read ≈ 1 at that resting film and pin
the outfall at a tailwater it can never drain below — a deadlock in
which the pipe cannot discharge and the cell cannot dry.

### 9.7.3 Cadence

By default the two domains **co-advance every routing step**: the 2D
solver advances over exactly \f$[t, t+\Delta t]\f$, and exchange volumes
reach the 1D side with at most one routing step of lag. This keeps
fill-and-spill coupling free of the batch-delay ringing that a longer
exchange interval produces at weir and culvert ponds.

`COUPLING_SYNC` batches the 2D advance over a longer span (clamped to
between one routing step and 60 s). It is a wall-clock lever for large
meshes, where per-routing-step advances degenerate into the tail
handling of §9.5.6, and it should be understood as trading accuracy for
speed: the held-exchange error grows with the span.

### 9.7.4 Units

The 2D solver runs internally in SI — metres, m³, m³/s, \f$g = 9.80665\f$.
The 1D engine always computes internally in **feet**, for every project,
US or metric: its reader converts metric input to feet on load and
converts back only at the display boundary. The 1D↔2D coupling factors
are therefore always the feet–metres conversion, independent of
`FLOW_UNITS`.

The mesh is different: it is authored in the project's display length
units, so it is scaled to SI on load for US projects and left alone for
metric ones. A mesh file may declare `;; UNITS: SI (m)` to assert it is
already metric regardless of `FLOW_UNITS`. Boundary stages share the
mesh's datum and scale with it; boundary flows scale with `FLOW_UNITS`.

Getting this wrong is silent and severe — an earlier version tied the
coupling factors to `FLOW_UNITS`, which collapsed them to 1.0 on metric
projects and left every coupled head off by 3.28× and every exchanged
volume off by 35×.

### 9.7.5 The exchange algorithm step by step

**Parameters.** Each coupling point carries the SWMM node index, the
discharge coefficient \f$C_d\f$ (default `COUPLING_CD`, 0.65), the
exchange area \f$A\f$ and, for outfalls, the flap-gate flag. A vertex row
that authors no area defaults to 1.0 in mesh area units (scaled to m²
with the mesh); with `COUPLING_AREA AUTO`, unauthored areas are
derived at resolve time as \f$\mathrm{clamp}(1.25 \times A_{conduit,max},\ 0.05,\ 2.0)\f$ m², where \f$A_{conduit,max}\f$ is the
full-flow area of the largest conduit connected to the node. A
vertex-coupled point records the first triangle incident on its
vertex as its host cell; that cell is pinned to tier 0 (§9.5.6).

**The regularized exchange law.** The pieces of (9-18), with all heads
in the 2D metre frame (\f$h_{1D} = 0.3048 \times\f$ the node head, crown
\f$z_{cr} = 0.3048 \times (z_{inv} + D_{full})\f$, \f$h_{max} = \max(h_{1D}, h_{2D})\f$) and \f$\varepsilon_o = 0.02\f$ m:

| | | | |
|---|---|---|---|
| \f[\varphi(x) = \sqrt{x}\f] | \f$x \ge \varepsilon_{o}\f$ | (9-27a) | |
| \f[\varphi(x) = \frac{3\,x}{2\sqrt{\varepsilon_{o}}} - \frac{x^{2}}{2\,\varepsilon_{o}^{3/2}}\f] | \f$0 \le x < \varepsilon_{o}\f$ | (9-27b) | |

which matches \f$\sqrt{x}\f$ in value and slope at \f$\varepsilon_o\f$ and has
the finite slope \f$3/(2\sqrt{\varepsilon_o})\f$ at zero. The effective
area grows linearly from the inlet area at the crown to twice it 5 cm
above,

| | | | |
|---|---|---|---|
| \f[A_{eff} = A\left\lbrack 1 + \min\!\left( 1,\ \frac{h_{max} - z_{cr}}{0.05} \right) \right\rbrack\f] | \f$h_{max} \ge z_{cr}\f$; else \f$A_{eff} = A\f$ | (9-28) | |

and the capped-pipe gate and the wet/dry ramps multiply the orifice
flow by cubic Hermite smoothsteps over the same scales,

| | | | |
|---|---|---|---|
| \f[Q \leftarrow Q\,\sigma(c), \qquad c = \mathrm{clamp}\!\left( \frac{h_{max} - z_{cr}}{0.05},\ 0,\ 1 \right)\f] | gate | (9-29a) | |
| \f[Q \leftarrow Q\,\sigma\!\left( \mathrm{clamp}\!\left( d_{src}/h_{dry},\ 0,\ 1 \right) \right)\f] | wet/dry ramp | (9-29b) | |

with \f$\sigma(t) = t^2(3 - 2t)\f$ and \f$h_{dry} =\f$ `DRY_DEPTH`. The gate
reads exactly zero at the crown, so a capped node exchanges nothing
until one side surcharges past it. For a drain (\f$Q > 0\f$) the source
depth \f$d_{src}\f$ is the maximum depth over the vertex stencil (or the
single cell for centroid coupling); for a spill it is the node depth,
converted. The driving head \f$h_{2D}\f$ is the wet-masked, depth-weighted
stencil mean under the VFR closure, the pseudo-Laplacian vertex head
under `FLAT`, and the cell head for centroid coupling.

**Caps.** Inside the marcher, at each tier-0 substep of length
\f$\Delta t_0\f$:

| | | | |
|---|---|---|---|
| \f[Q_{drain} \le \frac{\beta \max(V_{c}, 0)}{\Delta t_{0}}\f] | drain | (9-30a) | |
| \f[\lvert Q_{spill} \rvert\,\Delta t_{0} \le V_{node}\,f_{v} - D_{node}\f] | spill | (9-30b) | |

where \f$\beta = 0.8\f$, \f$V_c\f$ is the live coupling-cell volume, \f$f_v\f$ the
ft³-to-m³ factor and \f$D_{node}\f$ the volume already drawn from that
node during the current advance — the same stored water cannot spill
twice within a routing step. The capped \f$Q\,\Delta t_0\f$ is applied
directly to the coupling cell's volume (floored at zero) and
accumulated into the point's ledger \f$\int Q\,dt\f$.

**One routing step, in order:**

1. *Pre-routing.* For every coupled outfall, cache the 2D stage — the
   head of the deepest cell in the vertex stencil, converted to feet —
   and a wet/dry factor \f$\sigma(\mathrm{clamp}((d_{2D} - h_{dry})/h_{dry}, 0, 1))\f$ keyed on depth in excess of `DRY_DEPTH`
   (§9.7.2). The outfall boundary logic applies
   \f$\max(h_{standard}, h_{2D})\f$ inside every dynamic-wave iteration,
   blending by the cached factor and honouring flap gates.
2. *1D routing.* Each coupled junction's head sensitivity \f$G\f$ of
   (9-19) is scattered into the node's \f$\sum dQ/dH\f$ denominator every
   iteration (converted by \f$f_{Q,2D \to 1D} \times f_{L,1D \to 2D}\f$,
   m³/s per m to ft³/s per ft). The previous batch's junction
   exchange volumes drain from the delivery queue as a uniform
   lateral-inflow rate over the batch span.
3. *Post-routing.* The routing step's span joins the pending batch;
   when the pending span reaches the sync span (one routing step by
   default; `COUPLING_SYNC` clamps to between one routing step and
   60 s) the co-advance batch fires:
   1. Save the batch-start state; it seeds the per-cell outfall
      withdrawal budgets.
   2. Accumulate each coupled outfall's net discharge this step,
      \f$Q_{net} = (Q_{in} - Q_{out}) \times f_{Q,1D \to 2D}\f$;
      withdrawals are capped by the remaining budget of the cells the
      point taps. The batch total is injected as a constant-rate
      `coupling_flux` source, scattered over the vertex stencil with
      upwind-HGL weights — downhill cells for a source, uphill for a
      sink — normalized to unity, with the geometric
      partition-of-unity weights as the flat-surface fallback.
   3. Refresh rainfall, evaporation and forcing overrides on a 30 s
      cadence (immediately when the forcing API has marked the state
      dirty); resolve boundary time series and rating curves every
      step.
   4. Advance the marcher over the batch. Junction exchange is
      evaluated live at tier-0 cadence — Eq. (9-18) with
      (9-27)–(9-30) against the current 2D surface and the 1D heads
      frozen at batch start — and applied immediately.
   5. Book the ledgers: each point's \f$\int Q\,dt\f$ converts by
      \f$f_{Q,2D \to 1D}\f$ into the per-node exchange volume; the batch's
      boundary-edge volumes accumulate from the published window-mean
      fluxes; the 2D mass balance then ingests rainfall, evaporation,
      junction, outfall and boundary terms — every one the applied
      (post-cap) volume.
   6. Move the junction volumes to the delivery queue for step 2 of
      the following batch, clear one-shot forcings, and reset the
      window accumulators.

**Forcing override surface.** The C API can replace or augment the
computed exchange per cell: `swmm_2d_force_coupling_flux(engine, idx,
value, mode, persist)` prescribes a coupling rate (m/s, positive into
the 2D domain) with `mode` selecting override or add and `persist`
selecting one-shot or persistent application; `swmm_2d_force_rainfall`
/ `swmm_2d_force_evap` (and their `_uniform` variants) do the same for
the meteorological sources, and `swmm_2d_force_clear_all` clears every
prescription. One-shot prescriptions expire after the step they apply
to; any change marks the forcing state dirty so it takes effect on the
very next batch regardless of the 30 s refresh cadence.

Implementation: the exchange law and its sensitivity are
@ref openswmm::twoD::computeNodeCouplingQ and
@ref openswmm::twoD::computeNodeCouplingDQdh1d
(`src/engine/2d/coupling/NodeCoupling.cpp`, which also holds the
file-local `orificePhi`, `effectiveArea`, `wetVertexEta`,
`scatterCouplingFlux`, `budgetAvail`/`budgetDraw`/`budgetCredit`
helpers); outfall accumulation and tailwater caching are
@ref openswmm::twoD::accumulateOutfallDischargeStep and
@ref openswmm::twoD::updateOutfallBoundaries; the in-marcher exchange
loop is the tier-0 tail of
@ref openswmm::twoD::ExplicitInertialSolver::fireCells; the batch
orchestration and booking are
@ref openswmm::twoD::SurfaceRouter2D::advancePostRouting,
@ref openswmm::twoD::SurfaceRouter2D::coAdvanceStep and
@ref openswmm::twoD::SurfaceRouter2D::accumulateMassBalance; the
forcing entry points are declared in
`include/openswmm/engine/openswmm_2d.h`.

## 9.8 Rainfall and evaporation on the mesh

Rainfall reaches the mesh from the project's rain gages, mapped by
`RAINFALL_MODE`:

- **`NATURAL_NEIGHBOUR`** (default) interpolates the located gages onto
  every cell centroid — natural-neighbour (Laplace) weights inside the
  convex hull of the gages, inverse-distance weighting with power 2
  outside it. Laplace weights reproduce a linear rainfall field exactly
  within the hull and require no polygon-area integration: the weight
  for gage \f$g\f$ is the length of the shared Voronoi facet divided by the
  distance to \f$g\f$. The Delaunay triangulation of the gage sites is built
  by Bowyer–Watson; gage counts are small enough that this needs no
  external geometry library. The weights are static — gage positions do
  not change during a run — so they are built once and applied each step
  as a sparse weighted sum. Degenerate cases fall back cleanly: one
  gage everywhere, two or collinear gages by inverse distance, no
  located gage at all to the `SYSTEM` mean.
- **`SYSTEM`** applies the arithmetic mean of all gages uniformly.
- **`NONE`** applies no rain to the mesh.

**`NONE` is not an optimization, it is a modelling decision.** If the
project's subcatchments already convert the storm to runoff and deliver
it to nodes, rain on the mesh double-counts the same storm. Use `NONE`
whenever the surface is meant to receive only what the network gives it.

Evaporation applies the project's demand rate as a sink, tapered by a
smoothstep below `DRY_DEPTH` so a drying cell cannot evaporate more
water than it has.

There is no infiltration on the mesh (§9.12).

## 9.9 Reporting

The 2D domain carries its own mass balance, printed as a separate block
of the status report because its sign conventions are opposite to the 1D
routing balance:

```
  2D Surface Routing Continuity   cubic meters      10^6 ltr
  Initial Stored Volume ....
  Rainfall Inflow ..........
  1D -> 2D Spill Inflow ....
  Outfall Inflow ...........
  Boundary Inflow ..........
  2D -> 1D Drain Outflow ...
  Outfall Withdrawal .......
  Boundary Outflow .........
  Evaporation Loss .........
  Final Stored Volume ......
  Continuity Error (%) .....
```

Volumes are SI. Every term is booked from the volume actually applied —
after every cap, clamp and rescale — so the balance reflects the
volumes the solver actually applied.

A **2D Solver Statistics** block reports cumulative substeps,
face-kernel evaluations, mean and last internal step, the minimum, mean
and maximum active-cell fraction over the rebuild samples, and the
occupancy share of each local-time-stepping tier. Those last two are the
diagnostic for the two performance mechanisms of §9.5.6 and §9.5.7: a
mesh with 100 % active cells is telling you the active set is not
helping, and a tier histogram concentrated in tier 0 is telling you the
same about local time stepping.

Per-cell results — depth, free-surface elevation, velocity, gradients,
maximum-depth and maximum-velocity envelopes, cumulative volume and a
per-cell continuity residual — are refreshed on a report-scale cadence
rather than every routing step, because the six full-mesh passes cost
several times the solver's own advance on a large mesh, and nothing
consumes them faster than the reporting interval. Coupling and outfall
heads read the solver's live state directly, never these derived fields.

Two distinct vertex reconstructions exist and must not be confused. The
**solver** field is the pseudo-Laplacian stencil of Kumar et al. (2009),
built once from cell-centre geometry with Lagrange multipliers enforcing
linear exactness; dry cells contribute their bed elevations, which the
solver relies on. The **rendering** field is a wet-masked, depth-weighted
mean of the incident wet cells' free surfaces, with a wetted-contact
gate so that a cell votes at a corner only where its water actually
reaches it. Interpolating the solver field for display would drag water
surfaces up dry banks and down into thin films; interpolating the
rendering field in the solver would break the active-set logic.

With `OUTPUT_FILE` set in `[2D_OPTIONS]`, results are written to an
HDF5 file following the CF-1.11 and UGRID-1.0 conventions for
unstructured meshes — mesh topology, node and face coordinates, bed
elevations and roughness written once, then time-varying depth, head,
velocity and gradient fields appended. The file opens directly in
ParaView, QGIS, or any CF/UGRID-aware reader.

## 9.10 Verification against analytic solutions

The solver is verified against the SWASHES compilation of analytic
shallow-water solutions (Delestre et al., 2013), with the reference
formulas implemented independently of the engine. The figures below are
the relative \f$L^1\f$ depth error and the mass-balance error over the run;
steady cases are graded on the time mean of the final half of the
simulation.

| Case | SWASHES § | rel. \f$L^1\f$ depth error | mass error | graded against |
|---|---|---|---|---|
| Lake at rest, immersed bump | 3.1.1 | \f$6.3\times10^{-11}\f$ | \f$-7\times10^{-14}\f$ % | analytic |
| Lake at rest, emerged bump | 3.1.2 | \f$1.4\times10^{-16}\f$ | 0 | analytic |
| Subcritical flow over a bump | 3.1.3 | 0.67 % | \f$-6\times10^{-13}\f$ % | analytic |
| MacDonald 1000 m, subcritical | 3.2.1 | 2.0 % | \f$2\times10^{-11}\f$ % | analytic |
| Transcritical, no shock | 3.1.4 | 27 % | \f$-7\times10^{-13}\f$ % | baseline |
| Transcritical with shock | 3.1.5 | 12 % | \f$-8\times10^{-13}\f$ % | baseline |
| Stoker wet-bed dam break | — | 6.2 % | \f$-1\times10^{-12}\f$ % | baseline |
| Ritter dry-bed dam break | — | 10 % | \f$-3\times10^{-12}\f$ % | baseline |
| Thacker planar, 1D | — | 78 % | \f$7\times10^{-13}\f$ % | baseline |
| Thacker radial, 2D | — | 29 % | \f$-7\times10^{-14}\f$ % | baseline |
| Thacker planar, 2D | — | 43 % | \f$7\times10^{-14}\f$ % | baseline |
| MacDonald 1000 m, supercritical | 3.2.1 | 19 % | \f$-2\times10^{-13}\f$ % | expected failure |

Four observations follow.

**Well-balancedness is exact.** Both lake-at-rest cases sit at rounding,
including the emerged bump, which is a wetting and drying problem. The
C-property of §9.5.3 holds exactly.

**Mass conservation is exact.** Every case closes to \f$10^{-11}\f$ % or
better, which is round-off for the volumes involved. This is the
structural guarantee of (9-13) and (9-14), and it holds through wetting,
drying, positivity rescaling and boundary clamping.

**Accuracy is good where the approximation holds and degrades where it
does not.** The subcritical bump and the subcritical MacDonald channel
meet analytic tolerances. The transcritical, dam-break and oscillating
cases are graded against recorded baselines instead, because the
missing convective term is a physics limit rather than a discretization
error that a finer mesh would remove.

**The failure mode is specific and worth recognizing.** On the
subcritical bump, the computed free surface is dead flat over the crest,
where the analytic solution dips. That is not a defect: a flat \f$\eta\f$ is
the *exact* frictionless steady state of (9-2), because the dip is
\f$\Delta(v^{2}/2g)\f$ and there is no \f$q^{2}/h\f$ term to produce it. The
residual 0.7 % error is that dip and nothing else. The supercritical
MacDonald channel is recorded as an expected failure for the same
reason, one order more severely: with no convective inertia the fully
supercritical profile never steadies at all, and develops a roll-wave-like
unsteadiness. If your problem looks like that case, this is not the
solver for it.

## 9.11 Options

All keys live in `[2D_OPTIONS]`. Keys retired with the earlier implicit
integrator are accepted with a warning and ignored on file load, so
legacy models still open.

| Key | Default | Meaning |
|---|---|---|
| `INTEGRATOR` | `EXPLICIT` | The explicit local-inertial marcher is the only integrator. |
| `CFL_NUMBER` | 0.7 | \f$\alpha\f$ in (9-17). A true Courant fraction: 1.0 is the linear stability limit on any mesh. |
| `MAX_TIMESTEP` | 10 s | Cap on the marcher step; also caps the tier spread and the co-advance batch span. |
| `THETA` | 0.8 | Lateral blend (9-9). 1.0 is pure Bates et al. (2010); below 1 damps thin-film checkerboarding. |
| `FROUDE_MAX` | 1.5 | Face velocity clamp (9-10). |
| `LTS_TIERS` | 4 | Local-time-stepping tiers, 1–8. 1 forces a single global step. |
| `H_MOVE` | 0.003 m | Flux-activation depth (§9.5.7). Cells below it are source-only. |
| `DRY_DEPTH` | 0.001 m | Dry-cell threshold for face walls, evaporation taper and coupling ramps. |
| `CELL_CLOSURE` | `FLAT` | `FLAT` or `VFR` (§9.4). |
| `FACE_RECONSTRUCTION` | `MEAN` | `MEAN` or `VFR_FACE` (§9.5.2). |
| `VFR_MIN_WET_FRAC` | 0.01 | Wetted-fraction floor \f$\varepsilon\f$ of the regularized VFR closure, in \f$(0, 0.5]\f$. |
| `RAINFALL_MODE` | `NATURAL_NEIGHBOUR` | `NATURAL_NEIGHBOUR`, `SYSTEM` or `NONE` (§9.8). |
| `COUPLING_CD` | 0.65 | Default exchange discharge coefficient. |
| `COUPLING_AREA` | `DEFAULT` | `AUTO` derives an unauthored exchange area as clamp(1.25 × largest connected conduit full-flow area, 0.05, 2.0) m² (§9.7.5). |
| `COUPLING_SYNC` | 0 s | 0 co-advances every routing step; > 0 batches the 2D advance (§9.7.3). |
| `FLUX_DH_EPS` | 0.004 m | Head-gradient floor of the diffusive boundary flux. 0 restores the bare \f$\sqrt{\ }\f$. |
| `LIMITER_EPSILON` | \f$10^{-6}\f$ | Regularization of the output gradient limiter. |
| `REPORT_2D` | `YES` | Write 2D results. |
| `OUTPUT_FILE` | — | HDF5 results path, resolved relative to the `.inp` directory. |

The computational backend is selected at runtime rather than from the
input file. The built-in marcher is OpenMP-threaded on the host and uses
the project's `THREADS` setting; a Kokkos plugin (`omp`, `cuda`, `hip`
or `sycl`) is preferred when one is installed and the mesh is large
enough to amortize per-substep kernel launches. `OPENSWMM_2D_BACKEND`
overrides the choice. The gate is deliberately high — on a 25 000-cell
coupled model the OpenMP plugin measured an order of magnitude *slower*
than the built-in marcher, because the plugin pays a launch cost on
every substep while the built-in path is already threaded.

## 9.12 Limitations

- **No convective acceleration.** (9-2) omits it. Persistently
  supercritical flow, drawdown over a crest, and momentum-dominated
  contractions fall outside the model's validity rather than merely
  being under-resolved in it. §9.10 quantifies this.
- **No infiltration on the mesh.** Water on the surface leaves by
  flowing away, evaporating, or entering the network. Losses to the
  ground must be represented through the subcatchments.
- **The Froude clamp is a numerical device.** It
  bounds a velocity the momentum equation would otherwise leave
  unbounded. Results that sit on the clamp should not be regarded as
  physically meaningful.
- **Junction exchange is represented as an orifice.** The
  capped-pipe gate and its 5 cm transition band are a smooth
  approximation of grate hydraulics. Where inlet capacity governs, use the storm drain
  inlet models of §7.6 on the 1D side.
- **Geometry is planimetric.** Areas, lengths and normals are computed
  in plan (§9.3).
- **The mesh is fixed.** There is no adaptive refinement; adaptivity is
  in time (§9.5.6), never in space. Mesh quality is the modeller's
  responsibility, and it is not a cosmetic one: cell size enters the
  stable step through (9-4), so a handful of tiny cells at a coupling
  point can set the substep for the entire mesh.
- **Frozen 1D heads within a batch.** With `COUPLING_SYNC` > 0 the 1D
  heads a 2D advance sees are held for the batch. The error grows with
  the span.
- **Hot start files do not carry 2D state.**

## 9.13 References for this chapter

Bates, P. D., Horritt, M. S., and Fewtrell, T. J. (2010). "A simple
inertial formulation of the shallow water equations for efficient
two-dimensional flood inundation modelling." *Journal of Hydrology*,
387(1–2), 33–45.

Begnudelli, L., and Sanders, B. F. (2006). "Unstructured grid
finite-volume algorithm for shallow-water flow and scalar transport with
wetting and drying." *Journal of Hydraulic Engineering*, 132(4),
371–384.

Begnudelli, L., and Sanders, B. F. (2007). "Conservative wetting and
drying methodology for quadrilateral grid finite-volume models."
*Journal of Hydraulic Engineering*, 133(3), 312–322.

Belikov, V. V., Ivanov, V. D., Kontorovich, V. K., Korytnik, S. A., and
Semenov, A. Y. (1997). "The non-Sibsonian interpolation: A new method of
interpolation of the values of a function on an arbitrary set of
points." *Computational Mathematics and Mathematical Physics*, 37(1),
9–15.

Bowyer, A. (1981). "Computing Dirichlet tessellations." *The Computer
Journal*, 24(2), 162–166.

Bruwier, M., Archambeau, P., Erpicum, S., Pirotton, M., and Dewals, B.
(2017). "Shallow-water models with anisotropic porosity and merging for
flood modelling on Cartesian grids." *Journal of Hydrology*, 554,
693–709.

de Almeida, G. A. M., Bates, P., Freer, J. E., and Souvignet, M. (2012).
"Improving the stability of a simple formulation of the shallow water
equations for 2-D flood modeling." *Water Resources Research*, 48,
W05528.

de Almeida, G. A. M., and Bates, P. (2013). "Applicability of the local
inertial approximation of the shallow water equations to flood
modeling." *Water Resources Research*, 49(8), 4833–4844.

Delestre, O., Lucas, C., Ksinant, P.-A., Darboux, F., Laguerre, C., Vo,
T.-N.-T., James, F., and Cordier, S. (2013). "SWASHES: a compilation of
shallow water analytic solutions for hydraulic and environmental
studies." *International Journal for Numerical Methods in Fluids*,
72(3), 269–300.

Jawahar, P., and Kamath, H. (2000). "A high-resolution procedure for
Euler and Navier–Stokes computations on unstructured grids." *Journal of
Computational Physics*, 164(1), 165–203.

Kumar, M., Duffy, C. J., and Salvage, K. M. (2009). "A second-order
accurate, finite volume-based, integrated hydrologic modeling (FIHM)
framework for simulation of surface and subsurface flow." *Vadose Zone
Journal*, 8(4), 873–890.

Perot, B. (2000). "Conservation properties of unstructured staggered
mesh schemes." *Journal of Computational Physics*, 159(1), 58–89.

Sanders, B. F., Schubert, J. E., and Gallegos, H. A. (2008). "Integral
formulation of shallow-water equations with anisotropic porosity for
urban flood modeling." *Journal of Hydrology*, 362(1–2), 19–38.

Thacker, W. C. (1981). "Some exact solutions to the nonlinear
shallow-water wave equations." *Journal of Fluid Mechanics*, 107,
499–508.

Watson, D. F. (1981). "Computing the n-dimensional Delaunay tessellation
with application to Voronoi polytopes." *The Computer Journal*, 24(2),
167–172.


