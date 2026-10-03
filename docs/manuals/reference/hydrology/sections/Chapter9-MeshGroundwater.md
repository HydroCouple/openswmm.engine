@page hydrology_ref_ch9_mesh_groundwater Chapter 9: Spatially Explicit Groundwater on the 2D Mesh

@tableofcontents

## 9.1 Motivation and relation to Chapter 5

The two-zone aquifer of @ref hydrology_ref_ch5_groundwater "Chapter 5" is
integrated per subcatchment: one unsaturated moisture content and one
water-table depth each, lateral outflow by the empirical power function of
§5.3.5, no communication between neighbouring aquifers, and one feedback to
the rest of the engine — the end-of-step infiltration cap. Four physically
real processes are therefore not representable in it:

1. **Saturation-excess (Dunne) overland flow** where the water table reaches
   the ground, with rainfall rejected there.
2. **Return flow** — water that travelled laterally through the saturated zone
   and re-emerges downslope with no rain falling.
3. **Head-driven exchange between the aquifer and the pipes** — the mechanism
   behind groundwater inflow and sewer exfiltration, derived from heads rather
   than fitted per pipe.
4. **True capillary rise** meeting evaporative demand from below.

This chapter documents the opt-in kernel that supplies them: a vertically
integrated two-zone subsurface on the cells of the 2D mesh of
@ref hydraulics_ref_ch9_two_dimensional "Hydraulics Chapter 9", following the
semi-discrete finite-volume formulation of Qu and Duffy (2007). Each cell
carries a saturated thickness and an unsaturated store joined at a moving water
table; neighbouring saturated zones exchange by Darcy's law across the mesh's
own edges; nodes on the mesh exchange with the aquifer beneath them by a
conductance; and water a saturated column cannot hold returns to the surface
cell above it, where the shallow-water solver routes it.

**It is opt-in and changes nothing else.** The kernel runs only when a
`[2D_AQUIFER]` section is authored (or `[2D_OPTIONS] GROUNDWATER YES` says
so); every deck without one is unchanged and the Chapter 5 aquifers keep their
own path. The two cannot own the same water: a cell whose infiltration is
routed to a subcatchment aquifer (Chapter 8 §8.4) cannot coexist with a
`[2D_AQUIFER]`, and a storage node with a bed on the mesh has its `[STORAGE]`
exfiltration replaced by the node exchange of §9.6.

**`MODE PER_SUBCATCH`.** The design intends a second vehicle for the same
kernel — one degenerate cell per subcatchment, no lateral flux, exchange with
the outlet node. The code implements less: the token is parsed, written, and
removes the lateral edge topology, but the kernel still runs on the mesh cells
(the per-subcatchment cell list and outlet mapping are declared and never
populated). Today it means "the mesh kernel without lateral flow" (§9.11).

<!-- source: ROADMAP.md:178-190 (the four processes, opt-in); plans/TWO_ZONE_GROUNDWATER_FV_INTEGRATION_PLAN.md:166-178 (§2.7), :18 (manuscript); src/engine/2d/SurfaceRouter2D.cpp:895-904 (GROUNDWATER enable), :1054-1071 (one-owner refusal), :934-951 (storage exfiltration superseded); src/engine/2d/subsurface/SubsurfaceData.hpp:116-119 (PER_SUBCATCH intent); src/engine/2d/subsurface/SubsurfaceSolver.cpp:252-254 (what per_subcatch does); src/engine/2d/subsurface/SubsurfaceSolver.hpp:268-273 (subcatch_outlet_node_, cell_subcatch_ unused) -->

## 9.2 State variables and geometry

Everything in the kernel is SI; the authored `[2D_AQUIFER]` rows stay in the
project's units and are converted once, into the per-cell state (§9.12).

<!-- figure spec: one mesh cell in section: ground at z_c, aquifer bottom at z_bed = z_c − z_s, the water table at z_bed + h_g, the unsaturated column L = z_s − h_g drawn once as a bulk store h_u (closure A) and once as m sigma layers (closure B); arrows for q_plus at the top, q_0 across the table, deep loss at the bottom, lateral Darcy through the sides, node exchange to a pipe, Dunne return upward -->
![Figure 9-1](figures/png/hydrology_ch9_two_layer_column.png)

*Figure 9-1 One aquifer column and its fluxes, with the unsaturated zone held as a bulk store (closure A) and as sigma layers (closure B)*

Per cell \f$i\f$ of planimetric area \f$A_i\f$ and bed elevation \f$z_{c,i}\f$:

| Symbol | Meaning | Source |
|---|---|---|
| \f$z_s\f$ | Soil column thickness, bottom to surface (m) | `ZS` |
| \f$z_{bed} = z_c - z_s\f$ | Aquifer bottom elevation (m) | derived |
| \f$h_g \in [0, z_s]\f$ | Saturated thickness above the bottom (m); the saturated state | `HG0` seeds it; default 0 |
| \f$L = z_s - h_g\f$ | Unsaturated column thickness (m) | derived |
| \f$H = z_{bed} + h_g\f$ | Water-table elevation, the head for lateral flow and node exchange (m) | derived |
| \f$h_u\f$ | Closure A: bulk unsaturated storage as an equivalent depth of water (m) | seeded hydrostatic |
| \f$\theta_j,\ j = 0..m-1\f$ | Closure B: water content of σ layer \f$j\f$, layer 0 at the surface | seeded hydrostatic |
| \f$K_s, \theta_s, \theta_r, \alpha, \psi_b, \lambda, n, L_M, c_{loss}\f$ | Soil parameters (§9.3, §9.5) | row columns and keywords |

Water in the cell — the continuity ledger's storage term (§9.10) — is

\f[
W_i = \bigl(\theta_s\, h_g + S_{col}\bigr)\, A_i, \qquad
S_{col} = \begin{cases} h_u & \text{closures A and ENSLAVED} \\ \sum_j \theta_j\, L\, \Delta\sigma & \text{closure B} \end{cases}
\f]
(9-1)

Rows resolve onto cells by scope, `*` then `TAG` then `CELL`, and by authored
order within a scope; a residual content at or above the porosity is clamped
to half the porosity with a warning. The closure-B layer count \f$m\f$ is
global (`M_LAYERS`, default 8, range 2..128; a per-row value is parsed but
unused).

<!-- source: src/engine/2d/subsurface/SubsurfaceData.hpp:22-36, :142-169, :179-207; src/engine/2d/subsurface/SubsurfaceData.cpp:37-77 (defaults), :79-102 (storage()); src/engine/2d/subsurface/SubsurfaceSolver.cpp:115-174 (resolveRows: precedence :125-158, z_bed :166, theta_r clamp :167-172), :227-228, :234-250 (seeding); src/engine/2d/subsurface/SubsurfaceSections.cpp:183-251 -->

## 9.3 Soil characteristics

Four laws are selectable, globally through `[2D_AQUIFER_OPTIONS] SOIL_CHAR` or
per row. Each supplies an effective saturation \f$S_e(\psi)\f$, a water
content \f$\theta(\psi) = \theta_r + S_e(\theta_s - \theta_r)\f$ and a
relative conductivity \f$K_r(\psi)\f$, with suction \f$\psi \ge 0\f$ measured
upward from the water table (\f$\psi = L\f$ at the surface) and
\f$K(\psi) = K_s K_r(\psi)\f$:

| Law | \f$S_e(\psi)\f$ | \f$K_r(\psi)\f$ | Beyond \f$K_s, \theta_s, \theta_r\f$ |
|---|---|---|---|
| `GARDNER` (1958) | \f$e^{-\alpha\psi}\f$ | \f$e^{-\alpha\psi}\f$ | \f$\alpha\f$ |
| `RUSSO` (1988), default | \f$\bigl[(1 + \tfrac12\alpha\psi)\, e^{-\alpha\psi/2}\bigr]^{2/(2+L_M)}\f$ | \f$\bigl[(1 + \tfrac12\alpha\psi)\, e^{-\alpha\psi/2}\bigr]^{2}\f$ | \f$\alpha\f$, Mualem \f$L_M\f$ (0.5) |
| `BROOKS_COREY` (1964) | 1 for \f$\psi \le \psi_b\f$, else \f$(\psi_b/\psi)^{\lambda}\f$ | 1 for \f$\psi \le \psi_b\f$, else \f$(\psi_b/\psi)^{2 + 3\lambda}\f$ | \f$\psi_b, \lambda\f$ |
| `VAN_GENUCHTEN` (1980) | \f$\bigl[1 + (\alpha\psi)^{n}\bigr]^{-m},\ m = 1 - 1/n\f$ | \f$S_e^{L_M}\bigl[1 - (1 - S_e^{1/m})^{m}\bigr]^{2}\f$ | \f$\alpha, n, L_M\f$ |

Russo's retention is the Mualem-consistent inverse of his conductivity, so it
costs the same as Gardner and is the default; Brooks–Corey's air-entry branch
is the one non-smooth law; van Genuchten opens the Rosetta and UNSODA
databases. \f$S_e\f$ is kept in \f$[10^{-8}, 1]\f$.

<!-- figure spec: two panels over suction psi: S_e(psi) and K_r(psi) for the four laws at their defaults (alpha 2 per m, psi_b 0.2 m, lambda 0.4, n 1.6, L_M 0.5), Brooks–Corey showing its air-entry kink -->
![Figure 9-2](figures/png/hydrology_ch9_soil_characteristics.png)

*Figure 9-2 Retention and relative conductivity of the four soil-characteristic laws at their default parameters*

**Hydrostatic-equilibrium storage.** The water a column of thickness \f$L\f$
holds when hydrostatic above the table is

\f[ h_u^{\ast}(L) = \int_0^{L} \theta(\psi)\, d\psi = \theta_r L + (\theta_s - \theta_r)\, \sigma(L), \qquad \sigma(L) = \int_0^{L} S_e(\psi)\, d\psi \f]  (9-2)

with \f$\sigma(L)\f$ closed-form for Gardner, \f$(1 - e^{-\alpha L})/\alpha\f$,
and for Brooks–Corey,
\f$\psi_b + \psi_b\bigl[(L/\psi_b)^{1-\lambda} - 1\bigr]/(1 - \lambda)\f$ for
\f$L > \psi_b\f$ (else \f$L\f$; \f$\lambda = 1\f$ takes the logarithm), and by
four-point Gauss–Legendre quadrature for Russo and van Genuchten; \f$L\f$ is
floored at 1 mm. This integral is the ENSLAVED closure's whole unsaturated
state and the initial condition of every column.

**The closed-form recharge.** For Gardner, the quasi-steady recharge across the
table is Qu and Duffy's equation 22 verbatim, on the saturation scale
\f$\tilde h_u = (h_u - \theta_r L)/(\theta_s - \theta_r)\f$:

\f[ q_0(h_u, h_g) = K_s\, \frac{1 - e^{-\alpha L} - \alpha\, \tilde h_u}{\alpha L\,\bigl(1 - e^{-\alpha L}\bigr)}, \qquad L = z_s - h_g \f]  (9-3)

positive downward, negative for capillary rise, smooth in both states. It is
identical to a relaxation of the stored water toward its hydrostatic
equilibrium at a conductivity-set rate,

\f[ q_0 = C(L)\,\bigl[\tilde\sigma^{\ast}(L) - \tilde h_u\bigr], \qquad \tilde\sigma^{\ast}(L) = \frac{1 - e^{-\alpha L}}{\alpha}, \quad C(L) = \frac{K_s}{L\,\bigl(1 - e^{-\alpha L}\bigr)} \f]  (9-4)

**The other three laws generalise (9-4)**: the equilibrium is the law's own
\f$\sigma(L)\f$ from (9-2) and the rate is the conductivity at the column's
mean suction over that integral,

\f[ q_0 = \frac{\bar K}{\sigma(L)}\,\bigl[\sigma(L) - \tilde h_u\bigr], \qquad \bar K = K\!\left(\psi = \tfrac12 L\right) \f]  (9-5)

which for Gardner gives the same order and limits as (9-4).

**The Experimental caveat.** The code states, and this manual repeats: (9-5)
is the engine's generalisation, not the literature's. Gardner reproduces the
published closed form exactly and is verified against it. Russo, Brooks–Corey
and van Genuchten reproduce the correct equilibrium and sign, but their
relaxation *rate* is a modelling choice rather than a published result. The
closure-ladder benchmark and HYDRUS-1D comparison the plan schedules would
license them; until those run the three carry \status{Experimental} in §9.11,
Gardner or Russo are preferred for anything quantitative, and closure B — which
integrates the real Richards flux and needs none of this — is preferred where
the answer matters. The caveat concerns the recharge closure only.

**The dimensionless group** read by the closure selection is \f$\alpha L\f$,
the column thickness in capillary lengths; for Brooks–Corey the law's own
inverse length replaces \f$\alpha\f$ (\f$\alpha L \equiv L/\psi_b\f$).

**The wilting stress** reducing evaporative demand is a smooth Feddes-style
multiplier of the suction,

\f[ S(\psi) = \begin{cases} 1 & \psi \le 0 \\ s^{2}(3 - 2s),\ s = 1 - \psi/\psi_w & 0 < \psi < \psi_w \\ 0 & \psi \ge \psi_w \end{cases} \f]  (9-6)

deliberately not the legacy `if (infil <= 0)` gate and not piecewise-linear: a
kink here shows up as a kink in the ET ledger that reads like a solver artefact.

<!-- source: src/engine/2d/subsurface/SoilCharacteristic.hpp:22-72 (laws, q0, the @warning); src/engine/2d/subsurface/SoilCharacteristic.cpp:36-51 (floors, Gauss–Legendre), :63-98 (effectiveSaturation), :100-139, :141-180 (suctionAtSaturation), :204-227 (saturationIntegral), :229-249 (equilibriumStorage), :251-284 (rechargeQ0), :286-297 (alphaL), :299-310 (feddesStress); src/engine/2d/subsurface/SubsurfaceData.hpp:58-66, :107-110; plans/TWO_ZONE_GROUNDWATER_FV_INTEGRATION_PLAN.md:100-112 (eq. 22), :130-144 (§2.5) -->

## 9.4 Unsaturated-zone closures

The unsaturated store is represented per cell by `CLOSURE` (in
`[2D_AQUIFER_OPTIONS]` or on a row), resolved from `AUTO` at initialisation.

### 9.4.1 The AUTO rule

`AUTO` reads \f$\alpha L\f$ of the cell's *initial* column:

| \f$\alpha L\f$ | Closure | Reasoning |
|---|---|---|
| \f$< 1\f$ | `ENSLAVED` | The column equilibrates far faster than the table moves; its storage is a function of the table |
| \f$1 \le \alpha L < 5\f$, or `FORCE_CLOSED_FORM YES` | `CLOSED_FORM` | The quasi-steady recharge (9-3)/(9-5) holds |
| \f$\ge 5\f$ | `SIGMA` | The quasi-steady assumption fails: travel time, profile memory and a localised capillary fringe are the physics, and only a resolved column carries them |

A warning reports how many cells resolved to each. The design record also
describes a soft warning near \f$\alpha L \approx 3\f$ and a refusal of
`ENSLAVED` above \f$\alpha L = 2\f$; neither is in the code, which accepts
`CLOSURE ENSLAVED` on any column.

### 9.4.2 `CLOSED_FORM` (closure A)

One bulk state \f$h_u\f$ driven by the quasi-steady recharge. At a firing of
step \f$\Delta t\f$, with the table moving from \f$L_0\f$ to \f$L_1\f$ (§9.5),

\f[ h_u^{\,1} = h_u^{\,0} + \bigl(q^{+} - q_{ET} - q_0\bigr)\,\Delta t + \theta_{bot}\,(L_1 - L_0) \f]  (9-7)

where \f$q^{+}\f$ is the infiltration delivered from the surface (Chapter 8),
\f$q_{ET}\f$ the demand of §9.8, \f$q_0\f$ from (9-3) or (9-5), and the last
term the **handover** — the water the moving table carries across the zone
boundary. The slab content is the bulk store's own mean,
\f$\theta_{bot} = \mathrm{clamp}(h_u^{\,0}/L_0,\ \theta_r,\ \theta_s)\f$, and
it is the *same* number the saturated update pairs with its storage coefficient
\f$S_y = \theta_s - \theta_{bot}\f$; if the two zones disagreed on how much
water a moving table carries, the cell would leak at a rate proportional to
\f$\lvert \dot h_g \rvert\f$. Drained, it reduces to \f$\theta_s - \theta_r\f$.

Two caps keep the books. Above \f$\theta_s L_1\f$ the excess is *rejected* to
the surface (§9.7). Below \f$\theta_r L_1\f$ — a table rising faster than the
fluxes refill the column — the shortfall is drawn back up out of the saturated
zone in the same firing, lowering the table by
\f$\delta = (\theta_r L_1 - h_u^{\,1})/(\theta_s - \theta_r)\f$ (bounded by
its thickness); a shortfall no water anywhere can meet is booked as ET that was
never available rather than invented. The recharge is also capped by
availability: downward, \f$q_0 \le (h_u - \theta_r L_0)/\Delta t\f$; upward,
\f$-q_0 \le h_g(\theta_s - \theta_r)/\Delta t\f$. Without the first cap a cell
that cannot reach equilibrium drains past residual and keeps "delivering" at
\f$\bar K\f$ forever.

### 9.4.3 `ENSLAVED`

The shallow-water-table reduction of Qu and Duffy's equation 39: \f$h_u\f$ is
algebraic in \f$h_g\f$, \f$h_u = h_u^{\ast}(L)\f$ from (9-2), and the cell has
one state. Since \f$d h_u^{\ast}/dL = \theta(L)\f$, the column contributes its
*surface* water content as a storage debit, \f$S_y = \theta_s - \theta(L)\f$,
and infiltration drives the table with no lag: \f$q_0 = q^{+} - q_{ET}\f$.

The table is not updated by (9-10) alone. The balance
\f$W(h) = \theta_s h + h_u^{\ast}(z_s - h)\f$ is monotone, so the new table
solves \f$W(h_g^{\,1}) = W(h_g^{\,0}) + \Delta V_{sat}/A\f$ on \f$[0, z_s]\f$
by a safeguarded Newton with bisection fallback on the volume residual
(\f$W\f$ is flat across the capillary fringe, where only bisection works).
What the interval cannot hold is booked exactly: above \f$z_s\f$ as Dunne
excess, below 0 as a refund to the sinks (§9.5). The unguarded Newton this
replaced created 23.5 m³ on a two-cell exfiltration deck.

### 9.4.4 `SIGMA` (closure B)

An explicit column of \f$m\f$ layers on \f$\sigma = z/L \in [0, 1]\f$,
\f$z\f$ downward from the ground, \f$\Delta\sigma = 1/m\f$. Layers stretch and
compress as the table moves with no regridding and a fixed SoA stride, so a
layer sweep vectorises across columns. The **conserved** quantity is the water
depth per layer \f$w_j = \theta_j L \Delta\sigma\f$; \f$\theta\f$ is
re-derived from \f$w\f$ and the *new* \f$L\f$ at the end of the sweep, which
makes the compression exact. With \f$\dot L = (L_1 - L_0)/\Delta t\f$ and face
\f$j + \tfrac12\f$ at \f$\sigma_f = (j+1)/m\f$, the downward-positive flux is

\f[ F_{j+1/2} = K\!\bigl(\psi(\theta_j)\bigr) \;-\; \theta_{donor}\,\sigma_f\,\dot L \;-\; \frac{\bar D_{j+1/2}}{L_0\,\Delta\sigma}\,\bigl(\theta_{j+1} - \theta_j\bigr) \f]  (9-8)

Gravity is downward, so the advective donor is the layer above (first-order
upwind; MUSCL is a recorded follow-up). The grid term is upwinded on its own
sign: with \f$\dot L > 0\f$ (table falling) the donor is the layer below. The
capillary term, \f$D = K\, d\psi/d\theta\f$ harmonic-mean across the face, is
included only under `CAPILLARY_DIFF YES` (default off). The boundaries:

\f[ F_{top} = \min\!\bigl(q^{+},\ (\theta_s - \theta_0)\, L_0 \Delta\sigma / \Delta t\bigr) - q_{ET}^{taken}, \qquad F_{bot} = q_{0,phys} - \theta_{m-1}\,\dot L \f]  (9-9)

At the top, the first layer's headroom caps the infiltration accepted and the
remainder is **rejected** to the surface (§9.7); the ET taken is the demand
reduced by (9-6) at the top layer's own suction (wilting suction at 1 %
effective saturation) and limited to what the layer holds above residual. At
the bottom, \f$q_{0,phys}\f$ is the bottom layer's gravity drainage
\f$K(\psi(\theta_{m-1}))\f$ and the second term is the **handover** swept
across the moving boundary. Both together are what the saturated zone gains,
which is why (9-10) books \f$q_{0,phys}\f$ with
\f$S_y = \theta_s - \theta_{m-1}\f$ rather than \f$F_{bot}\f$ with
\f$\theta_s\f$: the same equation, but only the first conserves once the
column retains water above a falling table. The design record writes
\f$\theta_s\f$; \f$\theta_s - \theta_{bot}\f$ is the textbook specific yield
and what the code uses.

Positivity is a per-layer availability share: a layer may not export more than
it holds above residual in one step. After the update \f$\theta\f$ is
re-derived from the new geometry. A rising table compresses every layer, and a
layer whose water then exceeds \f$\theta_s L_1 \Delta\sigma\f$ is saturated:
the surplus cascades downward and whatever reaches the bottom is reported as an
**overflow to the saturated zone** applied to \f$h_g\f$ in the same firing; a
deficit below residual is drawn upward the same way. Clamping the surplus away
inside the sweep is the classic σ-grid failure and lost 0.39 m of water on the
closed-column oscillation gate. Columns are seeded hydrostatic above their
initial table, layer centres at \f$\psi = L(1 - \sigma)\f$.

<!-- source: src/engine/2d/subsurface/SubsurfaceData.hpp:68-74; src/engine/2d/subsurface/SubsurfaceSolver.cpp:176-208 (resolveClosures :188-191), :719-752 (per-closure q0 and Sy), :755-771 (availability caps), :785-834 (ENSLAVED bracketed solve), :894-944 (closure A update: handover :904-907, caps :908-941), :21-44 (specific-yield derivation); src/engine/2d/subsurface/SigmaColumn.hpp:22-71; src/engine/2d/subsurface/SigmaColumn.cpp:64-75 (seedHydrostatic), :114-279 (advanceColumn: interior :145-172, top :174-194, bottom :196-202, positivity :204-226, ALE cascade :228-278); src/engine/2d/subsurface/SubsurfaceSolver.cpp:234-250 (seeding); plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md:100-115 ("refused when αL > 2"), :185-193 (§2.4); plans/TWO_ZONE_GROUNDWATER_FV_INTEGRATION_PLAN.md:120-128 (eq. 39), :146-164 (§2.6) -->

## 9.5 Saturated zone: lateral Darcy flow and deep loss

### 9.5.1 The explicit finite-volume balance

At a cell's firing every saturated source shares one storage coefficient:

\f[ \Delta V_{sat} = \bigl(q_0 - q_{deep}\bigr)\, A\, \Delta t \;+\; V_{lat} \;-\; V_{node} \;+\; V_{link}, \qquad h_g^{\,1} = h_g^{\,0} + \frac{\Delta V_{sat}}{S_y\, A} \f]  (9-10)

where \f$V_{lat}\f$ is the lateral volume gathered from the cell's edge
accumulators since its last firing (positive in), \f$V_{node}\f$ the node
exchange gathered from its bed accumulators (positive out, §9.6),
\f$V_{link}\f$ the conduit seepage delivered (§9.6), and \f$S_y\f$ the
closure's specific yield of §9.4, floored at \f$10^{-3}\f$. The split is:
recharge from the current column state, then (9-10), then the clamp, then the
column sweep at the *clamped* \f$\dot L\f$, then the column's overflow or
deficit applied to \f$h_g\f$, then Dunne. Deriving \f$\dot L\f$ from the
clamped table is what makes the clamp harmless: no water is created at
\f$h_g = z_s\f$ or destroyed at \f$h_g = 0\f$.

**The clamp keeps its books.** A table above \f$z_s\f$ becomes Dunne volume
\f$(h_g^{\,1} - z_s)\, S_y A\f$ (§9.7). A negative table means the sinks asked
for more than the aquifer holds; the shortfall \f$-h_g^{\,1} S_y A\f$ is
refunded to the sinks that overdrew — deep loss first, then the node exchange
— and the refunded amounts are what the ledger records.

### 9.5.2 Lateral Darcy flow

Per unique interior edge \f$e\f$ between cells \f$l\f$ and \f$r\f$, of length
\f$\xi_e\f$ and centroid distance \f$d_e\f$,

\f[ Q_e = \bar K_e\, T_e\, \xi_e\, \frac{H_l - H_r}{d_e}, \qquad \bar K_e = \frac{2 K_{s,l} K_{s,r}}{K_{s,l} + K_{s,r}}, \qquad T_e = h_{g,\mathrm{donor}} \f]  (9-11)

positive from \f$l\f$ to \f$r\f$. The conductivity is the **harmonic mean**
(zero if either side is) and the transmissivity is the **donor** cell's
saturated thickness, upwind on the head difference — the mean here is the
classic way to make a drying aquifer pump water it does not have. Positivity is
a per-face share of the donor's drainable water, split across the donor's faces
so a cell with many wet neighbours cannot be emptied several times over:

\f[ \lvert Q_e \rvert \le \frac{\tfrac12\, T_e\, S_{y,\mathrm{donor}}\, A_{\mathrm{donor}}}{n_{f,\mathrm{donor}}\, \Delta t} \f]  (9-12)

The volume \f$Q_e \Delta t\f$ is booked \f$\mp\f$ into the edge's two side
accumulators at the face's cadence and gathered by each cell at its own firing
— the surface marcher's strategy, which makes conservation across tiers
inherited rather than re-proved (§9.9). Under `MODE PER_SUBCATCH` the edge
topology is empty and (9-11) is never evaluated.

### 9.5.3 Deep loss

\f[ q_{deep} = c_{loss}\, \frac{h_g}{z_s} \f]  (9-13)

a linear sink at full-saturation rate \f$c_{loss}\f$ (`C_LOSS`, default 0) —
exactly Chapter 5's deep percolation (5-21), with \f$c_{loss}\f$ as \f$DP\f$
and \f$h_g/z_s\f$ as \f$d_L/(E_G - E_B)\f$. The design record also allows a
`[GWF] DEEP` expression; the code implements the linear form only.

<!-- source: src/engine/2d/subsurface/SubsurfaceSolver.cpp:77-92 (constants), :448-491 (fireGwFaces: heads :458-461, donor T :463-468, K :470, Q :473, share :475-484, booking :486-488), :493-508 (gatherLateral), :660-687 (gather), :774-783 (saturated update), :836-863 (clamp with books), :865-893 (clamped L, overflow), :46-52 (split order); src/engine/2d/subsurface/SubsurfaceSolver.hpp:45-63; docs/manuals/reference/hydrology/sections/Chapter5-Groundwater.md:394-407 (5-21); plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md:65-99 (§2.1, "[GWF] DEEP") -->

## 9.6 Node–aquifer exchange

A 1D node on the mesh exchanges with the aquifer of the cell containing it
through a MODFLOW-River conductance, sign-determined by heads. With the pipe
head \f$H_{pipe} = z_{inv} + d\f$ (converted to metres) and the aquifer head
\f$H_{gw} = z_{bed} + h_g\f$,

\f[
Q_{node} = C\,\bigl(H_{gw} - H_{pipe}\bigr), \qquad
C = \begin{cases} \dfrac{K_c\, A_b}{d_C} & \text{semi-confining bed given (config b)} \\[1.2em] \dfrac{K_s\, A_b}{\tfrac12 z_s} & \text{no bed: direct Darcy over half the column (config a)} \end{cases}
\f]
(9-14)

positive out of the aquifer into the pipe. \f$K_c\f$ and \f$d_C\f$ are the
bed's conductivity and thickness (`KC`, `DC`); \f$A_b\f$ is `AREA`, or the
cell's area when omitted. Table above pipe head is groundwater inflow, pipe
head above table is exfiltration: one equation, no fitted coefficients.

<!-- figure spec: a manhole in section within a mesh cell: pipe head z_inv + d, water table z_bed + h_g, the semi-confining bed of thickness d_C and conductivity K_c between them, arrows for both exchange directions with the caps annotated -->
![Figure 9-3](figures/png/hydrology_ch9_node_bed_exchange.png)

*Figure 9-3 Node-aquifer exchange through a semi-confining bed, in both directions, with the caps that bound it*

**Caps.** A drain (\f$Q > 0\f$) may take at most half the cell's drainable
water per step, \f$\tfrac12 h_g S_y A / \Delta t\f$. A recharge
(\f$Q < 0\f$) is guarded three ways: a column whose table is within
\f$10^{-4} z_s\f$ of the ground takes nothing (it would come straight back as
Dunne excess and ping-pong against the frozen node head); the saturated zone's
headroom \f$(z_s - h_g)\, S_y A\f$ is taken in the same half-per-step share;
and the node cannot give more than it holds for the batch plus what flows
through it — a junction stores nothing below its rim but can lose water at the
rate it is fed.

**Cadence and booking.** The exchange is sampled at the marcher's tier-0
cadence against the batch-frozen 1D heads, like the surface's junction
exchange, and booked into the bed's accumulator; the aquifer cell gathers its
share when *it* fires, and neither side is pinned to the other's tier. Per
node, the batch's volume is added to the node's coupling volume for the next
routing step's lateral-inflow assembly — without entering the surface ledger;
its home is the aquifer's `led_node`.

**Which nodes exchange.** `NODE_ENROLMENT AUTO` (default) enrols every node
whose `[COORDINATES]` fall in a mesh cell with a direct-Darcy bed over the
cell's area unless a `[2D_AQUIFER_NODE]` row names it; a row overrides with its
own bed (cell given explicitly, 1-based, or as `CELL AUTO`) or opts out with
`EXCHANGE NO`; `ROWS` limits exchange to the authored rows. One bed per node
is enforced; a bed off the mesh is a warning and the node exchanges with
nothing. A storage node with a bed has its `[STORAGE]` exfiltration switched
off, since both would lose the same water twice.

**Conduit seepage.** With `LINK_SEEPAGE AUTO` (default), every conduit with a
`[LOSSES]` seepage rate whose polyline crosses the mesh delivers its seepage
volume into the cells it crosses, weighted by the length inside each (exact
Cyrus–Beck clipping). The 1D side keeps booking the loss; the volume lands in
`led_link` and rides (9-10) as \f$V_{link}\f$. `NONE` keeps it a system loss.

<!-- source: src/engine/2d/subsurface/SubsurfaceSolver.cpp:519-593 (sampleNodeExchange: heads :531-532, conductance :534-540, drain cap :545-548, recharge guards :549-583, booking :586-590), :595-603, :616-623; src/engine/2d/subsurface/SubsurfaceData.hpp:76-94, :96-105, :126-137; src/engine/2d/subsurface/SubsurfaceSections.cpp:257-294 (row grammar), :450-536 (resolveSubsurface: CELL AUTO :465-491, auto-enrolment :494-518, one bed :521-529), :538-620 (resolveLinkSeepage); src/engine/2d/subsurface/SubsurfaceSolver.hpp:203-217; src/engine/2d/SurfaceRouter2D.cpp:921-951, :954-966, :1580-1597 (coupling_volume), :1683-1700 (seepage booking); plans/TWO_ZONE_GROUNDWATER_FV_INTEGRATION_PLAN.md:174-176 -->

## 9.7 Saturation excess (Dunne) return flow and rejected infiltration

Two physically distinct routes carry water back to the surface cell above, and
both are booked to the same per-cell accumulator:

- **Dunne excess** — the table itself arriving at the ground: whatever
  \f$h_g^{\,1}\f$ exceeds \f$z_s\f$ by, after (9-10) and the column's overflow,
  becomes \f$(h_g^{\,1} - z_s)\, S_y A\f$ of surface water (under `ENSLAVED`,
  the volume above \f$W(z_s)\f$) and the table is held at \f$z_s\f$.
- **Rejected infiltration** — water the top of the column would not take:
  under closure A the excess of (9-7) above \f$\theta_s L_1\f$, under closure B
  \f$q^{+} - F_{top}\f$ from (9-9). `ENSLAVED` has no column to reject from.

\f[ V_{\to surf} = \bigl[\,\mathrm{DUNNE}\,\bigr]\, V_{Dunne} + q_{rej}\, A\, \Delta t \f]  (9-15)

Together they *are* the mass balance, which is why `DUNNE` defaults on; the
flag exists for compatibility comparisons and disables only the first route.
The volume waits in the cell's surface-bound accumulator until the surface cell
fires and drains it as a source in its own budget (Chapter 8, equation 8-6).
A dry surface cell above a saturating column would otherwise hold that water
forever, so the marcher pins every cell with pending surface-bound water active
at each rebuild — the one seed of the active set that does not come from the
surface's own state, and what lets return flow re-emerge in a valley bottom
with no rain falling. Rainfall on such a cell simply lands on the surface,
where the column accepts none of it and the surface routes it.

<!-- source: src/engine/2d/subsurface/SubsurfaceSolver.cpp:836-844 (Dunne from the clamp), :805-810 (ENSLAVED), :888-893 (from the column overflow), :908-909 (closure A rejection), :946-960 (the two routes, DUNNE flag), :625-644 (markPendingSurface, takeToSurface); src/engine/2d/subsurface/SubsurfaceData.hpp:120-123; src/engine/2d/subsurface/SigmaColumn.cpp:174-182; src/engine/2d/solver/ExplicitInertialSolver.cpp:614-626 (pinning), :1347-1360 (gw_return); src/engine/2d/SurfaceRouter2D.cpp:2157-2171 (aquifer_in) -->

## 9.8 Evapotranspiration modes

`GW_ET` — in `[2D_AQUIFER_OPTIONS]`, or as a `[2D_OPTIONS]` alias folded into
the same option at open — selects how the subsurface meets evaporative demand:

| Mode | Capillary rise (negative \f$q_0\f$) | Demand at the column top |
|---|---|---|
| `NONE` (default) | Suppressed: a negative \f$q_0\f$ is zeroed | None |
| `CAPILLARY_RISE` | Allowed | None |
| `BOUNDARY_ET` | Suppressed | Yes |
| `BOTH` | Allowed | Yes |

**The demand** is the surface cell's evaporation rate of Chapter 8 §8.5 — zero
under the default `[2D_OPTIONS] EVAPORATION YES` with no forcing, the
project's `[EVAPORATION]` under `CLIMATE` — reduced by (9-6) at the column's
mean suction with the wilting suction at 150 m (about \f$-15\f$ bar):

\f[ q_{ET} = e_i\; S\bigl(\psi(\bar S_e)\bigr), \qquad \bar S_e = \mathrm{clamp}\!\left(\frac{h_u/L_0 - \theta_r}{\theta_s - \theta_r},\ 10^{-6},\ 1\right), \quad \psi_w = 150\ \mathrm{m} \f]  (9-16)

with \f$\psi(\bar S_e)\f$ the law's retention inverse. Under closure A and
`ENSLAVED` this \f$q_{ET}\f$ is removed from the column (or, enslaved, from the
table) and booked. Under closure B it is offered to the column sweep, which
applies its own stress at the *top layer's* suction with the wilting suction
at 1 % effective saturation and limits the take to what the top layer holds;
the amount actually removed is booked. The two stresses therefore compound
under `SIGMA` — an implementation detail of the column rather than a documented
design choice.

This replaces Chapter 5's three-parameter split — upper-zone fraction,
lower-zone depth and the `if (infil <= 0)` gate — with one boundary condition
at the top of the unsaturated zone and one physical path from below: there is
no separate lower-zone ET term, water reaches the top by capillary rise (the
negative branch of (9-3)/(9-5), or the σ column's upward propagation), and the
closure that governs recharge downward governs capillary rise upward. The
wilting point survives; the lower-zone depth does not. Capillary rise is capped
by what the table holds (§9.4.2) and counted as the negative share of recharge.

<!-- source: src/engine/2d/subsurface/SubsurfaceSolver.cpp:689-712 (demand, stress, psi_w), :753 (NONE zeroes capillary rise), :740, :870-881 (column ET), :897-899, :910-942; src/engine/2d/subsurface/SigmaColumn.cpp:184-193 (column-side stress at Se = 0.01); src/engine/2d/subsurface/SubsurfaceData.hpp:124-125; src/engine/2d/data/SolverOptions2D.hpp:326-333 (alias); src/engine/core/SWMMEngine.cpp:310-320 (alias fold); src/engine/2d/SurfaceRouter2D.cpp:1380-1401 (the surface evap_rate); plans/TWO_ZONE_GROUNDWATER_FV_INTEGRATION_PLAN.md:179-200 (§2.8); plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md:646-652 (§7) -->

## 9.9 Time stepping and local time stepping

The subsurface has no loop of its own. It joins the surface marcher's
halving-order tier ladder through four hooks — tier assignment at every
rebuild, a face phase and a cell phase per due tier, and a settle before any
re-tier — under two guarantees enforced by construction. **G-A,
independence:** a cell's tier comes only from its own bound
\f$\min(\Delta t_g, \Delta t_u)\f$; the surface's pin-to-tier-0 rule for
coupling cells is not applied, so a cell with node exchange, infiltration or
Dunne transfer keeps its tier and the cross-domain volume accumulates until it
fires. **G-B, conservation:** every flux channel books \f$\pm\Delta V\f$ into a
side accumulator at the producer's cadence and is gathered by the owner at its
own firing — lateral Darcy on the edges, node exchange on the beds,
infiltration and seepage per cell, Dunne on the surface-bound accumulator.

**The saturated bound** is that of explicit diffusion on the head:

\f[ \Delta t_g \le C_{gw}\, \frac{A\, S_y}{\displaystyle \sum_e \bar K_e\, T_e\, \xi_e / d_e \;+\; c_{loss} A / z_s \;+\; \sum_{beds} C_b}, \qquad T_e = \max\bigl(h_{g,i},\ h_{g,j},\ 0.01\, z_s\bigr) \f]  (9-17)

with \f$S_y = \theta_s - \theta_r\f$ (floored), the wetter of the pair with a
floor for the transmissivity so an empty aquifer does not report an infinite
step and then take one, and the deep-loss and node conductances folded in as
linear sinks. `C_GW` defaults to 0.5, range (0, 1]. For realistic
conductivities the bound is minutes to hours — orders above the surface tiers,
which is what local time stepping exploits.

**The unsaturated bound** depends on the closure. A σ column's own bound is

\f[ \Delta t_u \le C_{col}\, \min_j \frac{L\, \Delta\sigma}{c(\theta_j) + \lvert \sigma_j \dot L \rvert + 2\bar D_j / (L\Delta\sigma)}, \qquad c(\theta) = \frac{dK}{d\theta} \f]  (9-18)

with the celerity by a centred difference through the retention inverse and
\f$\dot L\f$ estimated from the previous firing, \f$-q_{0,last}/S_y\f$.
Closure A is a relaxation whose stiffness \f$C = -\partial q_0/\partial h_u\f$
is taken numerically, so \f$\Delta t_u \le C_{col}/C\f$ covers every soil law;
`ENSLAVED` has no unsaturated bound. `C_COL` defaults to 0.9.

**Tiers and ordering.** A cell's rung is
\f$\lfloor \log_2(\Delta t_{cell}/dt_0) \rfloor\f$ by the same `ilogb`
arithmetic the surface uses, clamped to the ladder. The base step is the
minimum of the surface's and the aquifer's finest bound, so a groundwater cell
finer than every wet surface cell (or a whole domain with a dry surface) does
not fire beyond its bound on rung 0. The ladder height is the surface's
`LTS_TIERS` (default 4, at most 8); a slower cell lands on the coarsest rung
and fires more often than it needs — the design record's runtime tier count
(D-N1) is deliberately not taken, and `requiredTiers()` is unused. A face
fires at the finer of its two cells' rungs. Within a base substep the order is
surface faces, subsurface faces, surface cells, subsurface cells (so a cell
that infiltrated hands the water down within the substep), then node sampling
at tier 0. A routing window that does not fit a whole macro cycle ends with
global tail steps in which every aquifer rung fires once.

**Settling** does not apply pending volumes: turning a volume into a table
rise also moves the unsaturated store, so all volume goes through the cell
firing. Nothing strands, since accumulators are gathered through incidences a
re-tier does not change; continuity audits between firings must add them (§9.10).

<!-- source: src/engine/2d/subsurface/SubsurfaceSolver.hpp:23-43 (hooks, G-A, G-B); src/engine/2d/subsurface/SubsurfaceSolver.cpp:312-389 (refreshDtCell: Δt_g :327-360, Δt_u :362-386), :391-400 (requiredTiers), :402-442 (assignTiers: ilogb :412-418, no pinning :420-426, face rule :429-441), :298-303, :965-971, :973-1003 (settle); src/engine/2d/subsurface/SubsurfaceData.hpp:113-114; src/engine/2d/subsurface/SubsurfaceSections.cpp:141-148; src/engine/2d/solver/ExplicitInertialSolver.cpp:728-737 (dt0 fold), :808-818 (ladder height, D-N1 not taken), :2077-2111 (macro-cycle order), :2219-2233 (tail); src/engine/2d/data/SolverOptions2D.hpp:389; plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md:196-283 (§3.0, D-N1) -->

## 9.10 Continuity ledger and reporting

### 9.10.1 The ledger

The kernel keeps cumulative domain totals in m³:

| Term | Meaning | Sign |
|---|---|---|
| `infil_in` | \f$q^{+}\f$ delivered from the surface | in |
| `link` | Conduit seepage delivered | in |
| `lateral` | Net lateral Darcy across the domain edge | net in |
| `deep` | Deep percolation | out |
| `node` | Node exchange | out of the aquifer |
| `et` | Subsurface ET | out |
| `dunne` | Saturation excess and rejection returned to the surface | out |
| `recharge`, `caprise` | Unsaturated to saturated transfer and its negative share | internal |
| `init_storage`, `storage` | Water at the start; water now | — |

Three storage views exist. `storage()` is (9-1) summed over cells. The
**live** storage adds every side accumulator (lateral edge pairs, water in
flight to or from the surface, seepage in flight) and is what the results file
and the C API report. The **ledgered** storage adds only the lateral
accumulators, since the surface accumulators are always one firing out of
phase with `infil_in` and `dunne`. The continuity residual is

\f[ \mathcal{R} = S_{ledgered} - S_0 - \bigl[(F_{in} + Q_{lat} + F_{link}) - (F_{deep} + F_{node} + F_{ET} + F_{Dunne})\bigr] \f]  (9-19)

zero to machine precision for a conserving kernel; recharge and capillary rise
are internal to a cell and do not appear.

### 9.10.2 The report

When `[REPORT] CONTINUITY` is on and the kernel ran, a **2D Aquifer
Continuity** block follows the 2D Surface Routing Continuity block, in cubic
metres: Initial Stored Volume, Infiltration Inflow, Conduit Seepage Inflow,
Lateral Net Inflow, Deep Percolation, Node Exchange Outflow, Subsurface ET,
Saturation Excess Return, Final Stored Volume (the live storage), then Recharge
and Capillary Rise listed as internal. Its continuity error is (9-19) as a
percentage of \f$S_0 + F_{in} + F_{link} + \max(0, Q_{lat})\f$. By design the
surface block's Infiltration Loss (Chapter 8 §8.6) is this block's
Infiltration Inflow, and this block's Saturation Excess Return is the
surface's aquifer inflow term.

### 9.10.3 The results file

With the `GROUNDWATER` group of `REPORT_2D_VARIABLES` (in `DEFAULT`), the
results file carries per-cell fields held from each cell's last firing:

| Dataset | Content | Units |
|---|---|---|
| `Mesh2_face_gw_table_elev` | Water-table elevation \f$z_{bed} + h_g\f$ | m |
| `Mesh2_face_gw_hg` | Saturated thickness | m |
| `Mesh2_face_gw_hu` | Closure A unsaturated storage (0 under closure B) | m |
| `Mesh2_face_gw_recharge` | \f$q_0\f$, positive down, negative capillary rise | m/s |
| `Mesh2_face_gw_lateral` | Net lateral Darcy into the cell | m³/s |
| `Mesh2_face_gw_node_exchange` | Exchange with the node bed, positive into the pipe | m³/s |
| `Mesh2_face_gw_deep` | Deep percolation | m/s |
| `Mesh2_face_gw_et` | Subsurface ET | m/s |
| `Mesh2_face_gw_dunne` | Saturation excess returned to the surface | m³/s |
| `Mesh2_face_gw_infil_in` | Infiltration delivered from the surface | m/s |
| `Mesh2_face_gw_link_seepage` | Conduit seepage delivered | m³/s |

plus two static descriptors, `Mesh2_face_gw_bed_elev` and
`Mesh2_face_gw_closure` (0 closed form, 1 enslaved, 2 sigma); the domain
series `groundwater_ledger` of shape [time, 12] in the order recharge, lateral,
deep, node, dunne, caprise, et, infil_in, init_storage, storage, link,
continuity_residual (read its `terms` attribute — the seepage term was
appended and moved the residual); and `groundwater_node_exchange_cum`, the
cumulative exchange per bed, positive out of the aquifer, with node names as
an attribute. The `GW_DETAILED` group adds `Mesh2_face_gw_theta_sigma` of
shape [time, layer, face], layer 0 at the ground, zero for a non-sigma cell;
it is \f$m \times\f$ the cell count per step and off by default.

<!-- source: src/engine/2d/subsurface/SubsurfaceData.hpp:232-264 (ledger, the three storage views, residual contract); src/engine/2d/subsurface/SubsurfaceData.cpp:79-126; src/engine/2d/subsurface/SubsurfaceSolver.cpp:683-687, :859-863, :881, :899, :942, :958 (bookings); src/engine/plugins/DefaultReportPlugin.cpp:1003-1028 (2D Aquifer Continuity); src/engine/2d/output/Default2DOutputPlugin.cpp:906-1011 (fields :912-924, descriptors :933-961, ledger :962-977, beds :978-995, theta_sigma :996-1010); src/engine/2d/data/SolverOptions2D.hpp:194-199; include/openswmm/engine/openswmm_gw2d.h:100-110, :249-254 -->

## 9.11 Alternatives

| Family | Alternative | Behaviour | Status |
|---|---|---|---|
| Soil characteristic | `GARDNER` | Exponential retention and conductivity; recharge by equation 22 verbatim, verified against it | \status{Implemented} |
| Soil characteristic | `RUSSO` (default) | Mualem-consistent retention at Gardner's cost; recharge by (9-5), whose rate is the engine's | \status{Experimental} |
| Soil characteristic | `BROOKS_COREY` | Air-entry branch, closed-form equilibrium storage; recharge by (9-5) | \status{Experimental} |
| Soil characteristic | `VAN_GENUCHTEN` | Rosetta/UNSODA parameters, quadrature equilibrium; recharge by (9-5) | \status{Experimental} |
| Closure | `AUTO` | Per-cell selection from \f$\alpha L\f$ at 1 and 5 | \status{Implemented} |
| Closure | `CLOSED_FORM` | Bulk \f$h_u\f$ with quasi-steady recharge and the handover term | \status{Implemented} |
| Closure | `ENSLAVED` | \f$h_u\f$ algebraic in \f$h_g\f$; bracketed volume solve | \status{Implemented} |
| Closure | `SIGMA` | Explicit ALE σ column, first-order upwind | \status{Implemented} |
| Closure | `SIGMA` with MUSCL reconstruction | Recorded follow-up in the column's contract | \status{Planned} |
| Column option | `CAPILLARY_DIFF YES` | Harmonic-mean diffusive term in (9-8) and its bound | \status{Implemented} |
| Mode | `MODE MESH` (default) | Kernel on the mesh cells with lateral Darcy | \status{Implemented} |
| Mode | `MODE PER_SUBCATCH` | Today: the mesh kernel with lateral topology removed; the one-cell-per-subcatchment vehicle is not in the code | \status{Experimental} |
| Deep loss | Linear \f$c_{loss} h_g / z_s\f$ | Chapter 5's (5-21) form | \status{Implemented} |
| Deep loss | `[GWF] DEEP` expression | Named in the design record only | \status{Planned} |
| Node exchange | `NODE_ENROLMENT AUTO` | Every node inside the mesh, direct Darcy over the cell area | \status{Implemented} |
| Node exchange | `NODE_ENROLMENT ROWS` | Authored `[2D_AQUIFER_NODE]` rows only | \status{Implemented} |
| Node exchange | Semi-confining bed (`KC`, `DC`) | MODFLOW-River config b conductance | \status{Implemented} |
| Conduit seepage | `LINK_SEEPAGE AUTO` | Length-weighted delivery of `[LOSSES]` seepage into crossed cells | \status{Implemented} |
| Conduit seepage | `LINK_SEEPAGE NONE` | Seepage stays a system loss | \status{Implemented} |
| Return flow | `DUNNE YES` (default) | Table-at-ground excess to the surface | \status{Implemented} |
| Return flow | `DUNNE NO` | Compatibility only; rejection still returns | \status{Implemented} |
| ET | `GW_ET NONE`, `CAPILLARY_RISE`, `BOUNDARY_ET`, `BOTH` | §9.8 | \status{Implemented} |
| Time stepping | Shared tier ladder, height `LTS_TIERS` | §9.9 | \status{Implemented} |
| Time stepping | Runtime tier count reaching the slowest cell (D-N1) | `requiredTiers()` exists; the marcher does not grow the ladder | \status{Planned} |
| Verification law | Broadbridge–White | Exact Richards benchmark in the test harness; not selectable | \status{Retired} |
| Backend | GPU or OpenMP plugin marchers | The kernel runs on the CPU explicit marcher only; refused otherwise | \status{Planned} |

<!-- source: src/engine/2d/subsurface/SoilCharacteristic.hpp:62-72; src/engine/2d/subsurface/SigmaColumn.hpp:131-132; src/engine/2d/subsurface/SubsurfaceData.hpp:58-60, :107-140; src/engine/2d/subsurface/SubsurfaceSolver.cpp:252-254, :391-400; src/engine/2d/subsurface/SubsurfaceSolver.hpp:268-273; src/engine/2d/solver/ExplicitInertialSolver.cpp:812-815; src/engine/2d/SurfaceRouter2D.cpp:905-910 (backend refusal); plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md:88-90, :196-283 -->

## 9.12 Parameter estimation

A `[2D_AQUIFER]` row is `*`, `TAG name` or `CELL n`, then five positional
columns `KS ZS THETA_S THETA_R ALPHA`, then keyword pairs (so a fifth law
never renumbers a file). Values are authored in project units, converted once:

| Column or keyword | Symbol | US units | SI units | Default |
|---|---|---|---|---|
| `KS` | \f$K_s\f$ | in/hr | mm/hr | 1e-5 m/s internally |
| `ZS` | \f$z_s\f$ | ft | m | 5 m |
| `THETA_S`, `THETA_R` | \f$\theta_s, \theta_r\f$ | — | — | 0.45, 0.10 |
| `ALPHA` | \f$\alpha\f$ | 1/ft | 1/m | 2 /m |
| `PSI_B`, `LAMBDA` | \f$\psi_b, \lambda\f$ | ft, — | m, — | 0.2 m, 0.4 |
| `N`, `L` | \f$n, L_M\f$ | — | — | 1.6, 0.5 |
| `C_LOSS` | \f$c_{loss}\f$ | in/hr | mm/hr | 0 |
| `HG0` | initial \f$h_g\f$ | ft | m | 0 (empty aquifer) |
| `KC`, `DC`, `AREA` (node rows) | \f$K_c, d_C, A_b\f$ | in/hr, ft, ft² | mm/hr, m, m² | 0, 0, cell area |

Validation requires \f$K_s, z_s, \alpha, \lambda > 0\f$,
\f$0 < \theta_s \le 1\f$, \f$0 \le \theta_r < \theta_s\f$, \f$n > 1\f$,
\f$\psi_b, c_{loss} \ge 0\f$, and a positive `DC` wherever `KC` is given.

**Shared with Chapter 5.** The tables of §5.5 apply directly where the
parameter is the same physical quantity:

| This chapter | Chapter 5 | Where |
|---|---|---|
| \f$\theta_s\f$ | porosity \f$\varphi\f$ | §5.5.1 and its soil-texture tables and pedotransfer relations |
| \f$\theta_r\f$ | nearest analogue is the wilting point \f$\theta_{WP}\f$ (an upper bound on residual content) | §5.5.1 |
| \f$K_s\f$ | saturated hydraulic conductivity \f$K_S\f$ | §5.5.2 |
| \f$z_s\f$ | \f$E_G - E_B\f$, ground minus aquifer-bottom elevation | §5.2 |
| `HG0` | initial water-table depth \f$d_L\f$ | §5.2 |
| \f$c_{loss}\f$ | the deep-percolation coefficient \f$DP\f$ of (5-21) | §5.3.4 |

**Not carried over.** The percolation equation's conductivity and tension
slopes (§5.5.2) are replaced by the soil law's own \f$K(\psi)\f$; the ET
coefficients \f$UEF\f$ and \f$DEL\f$ (§5.5.3) by the boundary condition of
§9.8; the discharge constants \f$A1, B1, A2, B2, A3\f$ (§5.5.4) by the
head-driven exchange of §9.6. Calibrated values of these do not apply.

**New parameters.** \f$\alpha\f$ is the Gardner/Russo sorptive number, the
inverse capillary length (order 1 to 10 /m for sands, below 1 /m for clays; a
5 m column with \f$\alpha = 2\f$ /m starts at \f$\alpha L = 10\f$ and resolves
to `SIGMA` under `AUTO`). Brooks–Corey's \f$\psi_b, \lambda\f$ come from
engineering soil tests, van Genuchten's \f$\alpha, n\f$ from Rosetta or
UNSODA, and \f$K_c, d_C\f$ from a MODFLOW River package of the same area.

<!-- source: src/engine/2d/subsurface/SubsurfaceSections.cpp:183-251 (grammar, defaults, validation), :257-294 (node rows), :324-339 (unit factors); src/engine/2d/subsurface/SubsurfaceData.hpp:142-169, :281-286; src/engine/2d/subsurface/SubsurfaceSolver.cpp:140-155, :286-290; docs/manuals/reference/hydrology/sections/Chapter5-Groundwater.md:622-643, :394-407; plans/TWO_ZONE_GROUNDWATER_FV_INTEGRATION_PLAN.md:146-152 (αL regimes) -->

## 9.13 Implementation

| Concern | Where |
|---|---|
| State, options, rows, ledger | `src/engine/2d/subsurface/SubsurfaceData.hpp`, `.cpp` (`SubsurfaceState`, `GwOptions`, `GwAquiferRow`, `GwNodeBed`, `GwLinkShare`; the three storage views and the residual) |
| Soil laws, equilibrium storage, recharge, stress | `src/engine/2d/subsurface/SoilCharacteristic.hpp`, `.cpp` |
| The σ column | `src/engine/2d/subsurface/SigmaColumn.hpp`, `.cpp` (`advanceColumn`, `columnDtLimit`, `seedHydrostatic`, `columnStorage`) |
| The kernel | `src/engine/2d/subsurface/SubsurfaceSolver.hpp`, `.cpp` — `initialize()`; the hooks `refreshDtCell()`, `assignTiers()`, `fireGwFaces()`, `fireGwCells()`, `settle()`; `sampleNodeExchange()`; `bookInfiltrationFromSurface()`, `bookLinkSeepage()`, `takeToSurface()`; the per-cell `fireCell()` |
| Grammar, units, resolution, writer | `src/engine/2d/subsurface/SubsurfaceSections.cpp` — `[2D_AQUIFER_OPTIONS]`, `[2D_AQUIFER]`, `[2D_AQUIFER_NODE]`; `gwUnitFactors()`, `resolveSubsurface()`, `resolveLinkSeepage()`; documented in @ref engine_manual_sect_2D_AQUIFER_OPTIONS "[2D_AQUIFER_OPTIONS]", @ref engine_manual_sect_2D_AQUIFER "[2D_AQUIFER]" and @ref engine_manual_sect_2D_AQUIFER_NODE "[2D_AQUIFER_NODE]", with the `GROUNDWATER` and `GW_ET` keys in @ref engine_manual_sect_2D_OPTIONS "[2D_OPTIONS]" of @ref engine_manual_ch2_input_file "the input-file chapter" |
| Wiring into the router and marcher | `src/engine/2d/SurfaceRouter2D.cpp` `initialize()` (enable, resolve, `setSubsurface`, one-owner checks), `coAdvanceStep()` (node volumes to `coupling_volume`), `advancePostRouting()` (seepage booking); `src/engine/2d/solver/ExplicitInertialSolver.cpp` `syncAndRebuild()` (dt0 fold, tiers, pending-cell pinning), `runMacroCycle()` (hook order), `fireCellsImpl()` (infiltration down, return up) |
| Report and results | `src/engine/plugins/DefaultReportPlugin.cpp` (2D Aquifer Continuity); `src/engine/2d/output/Default2DOutputPlugin.cpp` (`Mesh2_face_gw_*`, `groundwater_ledger`, `groundwater_node_exchange_cum`) |
| C API | `include/openswmm/engine/openswmm_gw2d.h` (`swmm_gw2d_*`, `SWMM_GW2D_LED_*`); `src/engine/2d/api/ApiGw2D.cpp` |

The design records are `plans/TWO_ZONE_GROUNDWATER_FV_INTEGRATION_PLAN.md`
(manuscript summary, the four feedbacks) and
`plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md` (the explicit
formulation, the σ column, the LTS guarantees). Where this chapter and those
documents differ, the chapter follows the code, and the differences are named
where they arise: the specific yield \f$\theta_s - \theta_{bot}\f$ in place of
\f$\theta_s\f$ (§9.4.4), the closure thresholds without the
\f$\alpha L \approx 3\f$ warning or the `ENSLAVED` refusal (§9.4.1), the linear
deep loss without a `[GWF] DEEP` expression (§9.5.3), the fixed ladder height
in place of the runtime tier count (§9.9), and `MODE PER_SUBCATCH` as
implemented (§9.1).

<!-- source: src/engine/2d/subsurface/SubsurfaceSolver.hpp:97-230; src/engine/2d/SurfaceRouter2D.cpp:895-966, :1580-1597, :1683-1700; src/engine/2d/solver/ExplicitInertialSolver.cpp:417, :614-626, :728-737, :808-818, :1347-1357, :2077-2111; src/engine/2d/subsurface/SubsurfaceSections.cpp:300-318 (registration), :625-660 (writer) -->
