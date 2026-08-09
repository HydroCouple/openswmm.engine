# Chapter 8: Explicit Finite-Volume Analysis

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

Chapter 3 notes that although more powerful solution techniques are
available — among them shock-capturing finite volume schemes (Toro,
2001) — SWMM 5 continues to use EXTRAN's node-link approach. This
chapter documents the alternative OpenSWMM provides: a Godunov-type
explicit finite-volume solver, selected with `FLOW_ROUTING FV`.

It is an *addition*, not a replacement. Dynamic wave analysis
(Chapter 3) remains the default and remains the right choice for most
work. The finite-volume solver exists for the cases where the
node-link formulation is structurally, rather than incidentally,
limited: exact volume conservation, transcritical flow in steep
sewers, and the speed of a pressurization front.

## 8.1 What the method changes

Dynamic wave analysis carries one discharge per conduit and treats a
junction as a storage volume. Three consequences follow, and the
finite-volume method addresses each:

**Conservation.** The dynamic wave momentum equation is written in a
non-conservative form. Water volume is tracked well but not exactly,
and the routing continuity error is a diagnostic the modeller is
expected to watch. A finite-volume method in conservation form
conserves volume identically — the flux that leaves one control volume
is the same number that enters the next — so continuity error is a
property of the *reporting*, not of the solution.

**In-conduit resolution.** With one discharge per conduit, a backwater
profile or a bore *inside* a pipe cannot be represented at all. The
finite-volume mesh subdivides each conduit, so the flow field within a
reach is resolved.

**Discontinuities.** Pressurization fronts, hydraulic jumps and
transcritical transitions are genuine discontinuities. Their
propagation speed is set by the Rankine–Hugoniot conditions, which only
a conservative scheme reproduces (Hou and LeFloch, 1994). The dynamic
wave solver handles transcritical flow by *suppressing* it, through
inertial damping and normal-flow limiting.

What the method does **not** change: it is still one-dimensional, still
uses the Preissmann slot for pressurized flow and therefore still
cannot represent sub-atmospheric pipe pressure, and still treats a
general junction as a stagnation volume (§8.6).

## 8.2 Governing equations

The conservation form of the St. Venant equations for a channel or pipe
of arbitrary but prismatic cross-section is

| | | | |
|---|---|---|---|
| $$\frac{\partial A}{\partial t} + \frac{\partial Q}{\partial x} = q_{L}$$ | Continuity | (8-1) | |
| $$\frac{\partial Q}{\partial t} + \frac{\partial}{\partial x}\left( \frac{Q^{2}}{A} + gI_{1} \right) = gI_{2} + gA\left( S_{0} - S_{f} \right)$$ | Momentum | (8-2) | |

where $A$ is flow area, $Q$ discharge, $q_L$ lateral inflow per unit
length, $S_0$ bed slope, $S_f$ friction slope, and $I_1$ is the first
moment of the wetted area about the free surface,

| | | | |
|---|---|---|---|
| $$I_{1}(h) = \int_{0}^{h}{(h - \eta)\,T(\eta)\,d\eta} = \int_{0}^{h}{A(\eta)\,d\eta}$$ | | (8-3) | |

with $T$ the top width and $h$ the depth. The second equality follows
by differentiating under the integral, and it is the form OpenSWMM
tabulates: $I_1$ is simply the antiderivative of $A$. The term $I_2$
accounts for width variation along the conduit and vanishes for a
prismatic reach, which every SWMM conduit is.

Comparing (8-2) with the dynamic wave momentum equation (3-2), the
difference is that the pressure and convective terms appear *inside*
the flux divergence rather than as separate gradient terms. That is the
whole of the conservation property.

## 8.3 The computational mesh

The mesh is internal numerical discretization. It creates no named
objects, appears in no report, and is invisible to the model's
topology. Each conduit is divided into

| | | | |
|---|---|---|---|
| $$n = \max\left( n_{min},\ \left\lceil L/\Delta x_{target} \right\rceil \right)$$ | | (8-4) | |

cells of equal length, where $\Delta x_{target}$ is `FV_CELL_LENGTH`
and $n_{min}$ is `FV_MIN_CELLS`. `FV_MIN_CELLS` is a floor and applies
whether or not a $\Delta x$ target is set.

The default is `FV_MIN_CELLS 4`, `FV_CELL_LENGTH 0` — four cells per
conduit with no length target. **One cell per conduit is available but
is not a supported operating point.** A cell-centred scheme places the
cell's bed at its mid-point elevation, so a conduit meshed as a single
cell presents an artificial bed step of half the conduit's fall at every
manhole. On the EPA reference drainage model, mean absolute peak-flow
deviation from the dynamic wave solver against cells per conduit:

| cells per conduit | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| mean absolute peak-flow deviation | 37.1 % | 25.7 % | 15.3 % | 7.6 % |
| worst link | −75.8 % | −58.1 % | −43.2 % | −22.6 % |
| wall-clock, relative to one cell | 1.0× | 1.4× | 2.2× | 5.4× |

Four is the knee, not the answer: it more than halves the one-cell error
for about twice the cost, and convergence past it is slower than its
price. §8.10 gives the same comparison against $\Delta x$ targets. Set
`FV_CELL_LENGTH`, or raise `FV_MIN_CELLS`, whenever peak flows or
in-conduit profiles matter.

The conduit length used is the Courant-lengthened `mod_length`
(Chapter 3), so `LENGTHENING_STEP` acts as a $\Delta x$ floor for short
pipes exactly as it acts as a length floor for the dynamic wave solver,
and the bed slope and roughness the finite-volume solver uses are the
same adjusted values.

The mesh is fixed after initialization. There is no adaptive mesh
refinement; adaptivity is in time (§8.5.5), not space.

## 8.4 Cross-section closure and pressurized flow

Mixed free-surface and pressurized flow is handled with **no
regime-switching logic**. The Preissmann slot is folded into the
cross-section relations $A(h)$, $T(h)$ and $R(h)$, producing one
continuous geometry valid from dry bed to full pressurization. Every
cell evaluates the same flux function, and a "transition" is simply a
cell's depth crossing the crown.

### 8.4.1 Slot width and celerity

The slot width is derived from the pressurized wave celerity the user
asks for, rather than the other way round:

| | | | |
|---|---|---|---|
| $$T_{slot} = \frac{g\,A_{full}}{c_{slot}^{2}}$$ | | (8-5) | |

where $c_{slot}$ is `FV_SLOT_CELERITY`. Making the celerity the
user-facing quantity keeps the accuracy/cost trade explicit: because
the explicit time step is bounded by $\Delta x/(|v| + c)$, a physical
acoustic celerity of 1000 ft/s or more would collapse the step size.
The default of 100 ft/s is the same order the dynamic wave solver's
`SURCHARGE_METHOD SLOT` produces. The slot is additionally capped at
5 % of the section's maximum width so it can never become the dominant
storage and understate a surge.

### 8.4.2 The tapered slot mouth

The slot does not appear abruptly at the crown. It opens smoothly over
$[y_{c},\,y_{full}]$, with $y_c$ the crown cutoff of Chapter 3
(0.985257 $y_{full}$), through a ramp $\varphi$ that is $C^{1}$ at both
ends:

| | | | |
|---|---|---|---|
| $$T(h) = T_{x}(h) + T_{slot}\,\varphi(s), \qquad s = \frac{h - y_{c}}{y_{full} - y_{c}}$$ | | (8-6) | |
| $$\varphi(s) = s^{2}(3 - 2s) \quad\text{for } 0 < s < 1$$ | | (8-7) | |

where $T_x$ is the section's own top width. A discontinuous $dA/dh$ at
the crown would produce spurious reflections there and corrupt the
Riemann solver's wave-speed estimates; the taper is what prevents both.
The area is the exact integral of (8-6), so $A$ and $T$ remain a
consistent pair through the transition.

Open sections carry no crown and no taper: above `y_full` the section
simply continues with vertical walls of width $w_{max}$, which is one
code path with the closed case and keeps the celerity physical.

**Depressurization** is the same mechanism in reverse — the cell's
state re-enters the free-surface part of $A(h)$ — and the closure is
memoryless, so a pipe driven repeatedly over and back across the crown
returns to the same state each time. This is why the *static* slot is
used here rather than the Dynamic Preissmann Slot: a relaxing slot
makes bore speed depend on relaxation history, which would destroy the
Rankine–Hugoniot front speed the method exists to get right.

### 8.4.3 Inverting the closure

The solver carries $A$ and derives the free surface as
$\eta = z_b + h$, so it needs $h(A)$. This inverse is constructed from
the forward closure itself rather than from SWMM's `Y`-tables.

The reason is worth recording. SWMM's geometry tables are *independent*
tabulations: `A_Circ` gives area from depth and `Y_Circ` gives depth
from area, and they round-trip only to table resolution — measured at
0.016 ft on a 3 ft circular pipe near the crown, about 0.5 % of the
diameter. That is harmless for the dynamic wave solver, which never
composes them. Here it is not: cells at equal free surface but
different bed elevation would reconstruct *different* surfaces, and the
still-water property of §8.5.2 would fail on every partly-full pipe.

For the same reason, the tabulated top width is not the finite
difference of the tabulated area. `T` is therefore used only where the
physical top width is the quantity wanted — wave celerity and node
surface area — while the flux path uses $A$ and $I_1$, which are
mutually consistent by construction.

## 8.5 Numerical scheme

### 8.5.1 Face reconstruction

Cell states are reconstructed at each interface using the hydrostatic
reconstruction of Audusse et al. (2004):

| | | | |
|---|---|---|---|
| $$z^{*} = \max\left( z_{L},\,z_{R} \right)$$ | | (8-8) | |
| $$h_{K}^{*} = \max\left( 0,\ \eta_{K} - z^{*} \right), \qquad v_{K}^{*} = v_{K}$$ | | (8-9) | |

for $K \in \{L, R\}$. Velocity is preserved and the discharge
recomputed as $Q^{*} = A(h^{*})\,v$.

### 8.5.2 The still-water property

After the interface flux $\mathbf{F}$ is computed, each side receives a
correction before it is applied to its own cell:

| | | | |
|---|---|---|---|
| $$\mathbf{F}_{K}^{c} = \mathbf{F} + \begin{bmatrix} 0 \\ g\left( I_{1}(h_{K}) - I_{1}(h_{K}^{*}) \right) \end{bmatrix}$$ | | (8-10) | |

At rest $\eta_L = \eta_R$, the flux reduces to $[0,\ gI_1(h^{*})]$, and
(8-10) leaves exactly $gI_1(h_i)$ at *both* faces of cell $i$. The
momentum divergence is therefore identically zero for any bed profile:
a lake at rest is preserved to machine precision, including across
slope breaks, adverse slopes and while pressurized. This is the
"C-property", and it holds regardless of any quadrature error in the
$I_1$ table, since only single-valuedness is required.

### 8.5.3 Interface flux

The system $\mathbf{U} = [A,\ Q]^{T}$ is $2 \times 2$ with two
genuinely nonlinear fields and no middle wave, so the interface flux is
the HLL flux (Harten, Lax and van Leer, 1983):

| | | | |
|---|---|---|---|
| $$\mathbf{F} = \frac{S_{R}\mathbf{F}_{L} - S_{L}\mathbf{F}_{R} + S_{L}S_{R}\left( \mathbf{U}_{R} - \mathbf{U}_{L} \right)}{S_{R} - S_{L}}$$ | | (8-11) | |

with Davis signal-speed estimates and the standard dry-bed forms
$S_L = v_R - 2c_R$ (dry left) and $S_R = v_L + 2c_L$ (dry right), where
$c = \sqrt{gA/T}$.

A contact wave appears only when a third component is carried. For
$\mathbf{U} = [A,\ Q,\ A\varphi]^{T}$ the eigenvalues are $v - c$, $v$,
$v + c$, and $\lambda = v$ *is* the discontinuity that transports the
species. Its speed is therefore computed here and used by the transport
scheme (§8.8):

| | | | |
|---|---|---|---|
| $$S^{*} = \frac{S_{L}A_{R}\left( v_{R} - S_{R} \right) - S_{R}A_{L}\left( v_{L} - S_{L} \right)}{A_{R}\left( v_{R} - S_{R} \right) - A_{L}\left( v_{L} - S_{L} \right)}$$ | | (8-12) | |

`FV_RIEMANN` selects whether the species flux resolves this wave
(`HLLC`, the default) or averages it away (`HLL`, a diffusive
baseline). It has no effect on the hydraulics.

### 8.5.4 Friction, local losses and positivity

Manning friction is integrated semi-implicitly, so it imposes no time
step restriction of its own:

| | | | |
|---|---|---|---|
| $$Q^{n + 1} = \frac{Q^{*}}{1 + g\,\Delta t\,\left( n/\phi \right)^{2}\left| v \right|/R^{4/3}}$$ | | (8-13) | |

Conduit entrance and exit loss coefficients are applied at the
node-coupling faces in the same implicit form, so calibrated models
carry over unchanged.

Before the update, outgoing fluxes are scaled so that no control volume
can export more than it holds. The *identical* scaled flux updates both
neighbours, so this limiter cannot affect conservation. At rest every
flux is zero and the scale factor is one, so it cannot affect the
still-water property either.

### 8.5.5 Time stepping

The solver substeps internally to fill each routing step. The routing
step is therefore a reporting and forcing cadence, not a stability
constraint. Each substep is bounded by the Courant condition over every
face,

| | | | |
|---|---|---|---|
| $$\Delta t \leq \alpha\,\frac{\Delta x}{\left| v \right| + c}$$ | | (8-14) | |

with $\alpha$ = `FV_CFL`. Two details matter in practice.

**Boundary ghosts enter the census.** A surcharged manhole presents a
*pressurized* ghost state, whose celerity is the slot celerity, to a
part-full pipe whose own cells report a free-surface celerity twenty
times smaller. That configuration is ubiquitous in a real sewer, and a
census over cells alone over-runs the true limit by more than an order
of magnitude.

**Steps are re-checked after they are taken.** A cell can cross the
crown *inside* a substep, taking its celerity from the free-surface
value to the slot value. A step sized on the pre-step state is then far
too large precisely when the model is doing something interesting. The
solver re-runs the census on the state the substep produced and, if the
two disagree by more than a factor of two, rolls the step back and
retries with the smaller value. Every input to that decision is solver
state, so the retry sequence is deterministic.

Time integration defaults to forward Euler
(`FV_TIME_INTEGRATION EULER`), because overall accuracy is capped by the
spatial reconstruction and by the friction splitting. `RK2` selects
Heun's strong-stability-preserving two-stage method, applied to the
*whole* operator: two forward steps at the same $\Delta t$, averaged.
Averaging the operator rather than splitting it keeps the semi-implicit
friction and the positivity limiter inside each stage, where their
stability arguments hold. `RK2` and local time stepping are mutually
exclusive — tiering gives different volumes different steps, so the two
stages would be averaging states that never shared one.

### 8.5.6 Local time stepping

Condition (8-14) is *local*, but a single global substep applies the
smallest value found anywhere to every cell in the model. In a sewer
network that is expensive in a specific and avoidable way: pipe lengths
span two orders of magnitude, and a pressurized cell runs at the slot
celerity while its open-channel neighbour runs at $\sqrt{gA/T}$. One
5 m pipe, or one surcharged manhole, otherwise sets the step for ten
thousand others.

Local time stepping (`FV_LTS`, on by default) removes that coupling.
Each control volume is assigned a power-of-two **tier** from its own
stable step,

| | | | |
|---|---|---|---|
| $$k_{i} = \left\lfloor \log_{2}\left( \Delta t_{i}/\Delta t_{0} \right) \right\rfloor$$ | | (8-15) | |

where $\Delta t_{0}$ is the finest requirement in the model, and
advances at $2^{k}\Delta t_{0}$. A face fires at the finer of its two
sides. One **macro cycle** is $2^{K-1}$ base substeps, where $K$ is the
tier count; a tier-$k$ volume fires every $2^{k}$ of them, so every
volume advances the same total span.

Three properties make this a scheduling change rather than a different
scheme:

**Conservation across a tier interface is exact.** A face books
$\pm F\,\Delta t$ into *both* incident volumes' accumulators when it
fires; a volume drains what has accumulated when it fires. What leaves
a fine cell is therefore bit-for-bit what arrives in its coarse
neighbour. The naive alternative — letting the coarse side integrate
its own flux estimate — loses mass at every interface.

**Windows are aligned.** A tier-$k$ face opens its window at the start
of its interval and the volume it feeds closes its window at the end of
its own, so by the time a volume fires, every face bounding it has
booked exactly the span the volume is about to integrate its sources
over. Firing volumes at the *start* of their windows instead hands a
coarse manhole several base steps of lateral inflow against one base
step of drained outflow, and the resulting sawtooth in a small volume
is large enough to be visible in the water surface far from it.

**Tiers are graded.** No face may span more than one tier level. A
coarse cell placed directly against a much finer one holds a frozen
state through its whole window while its neighbour resolves a front
trying to cross into it, and the flux booked against that frozen state
overshoots. Grading is the standard admissibility condition for local
time stepping on an unstructured mesh, and it is what makes a filling
bore cross a length transition correctly.

Tiers are reassigned only at a macro-cycle boundary, never inside one:
a volume re-tiered mid-cycle would either skip a flux it is owed or
drain one at the wrong $\Delta t$.

Two cases fall through to global stepping, both deliberately. When
tiering finds nothing to separate — a uniform mesh carrying a uniform
state — the solver takes the untiered path and reproduces `FV_LTS NO`
to the last bit. And when the solver carries advected species, tiering
is disabled outright: the flux limiter of §8.8 bounds a cell's update
against the extrema of its whole neighbourhood in one synchronous
sweep, and under tiering those neighbours are at different times.

`FV_LTS_MAX_TIERS` caps the spread, at 6 by default (a 64× ratio).

What tiering reduces is *work*, not the substep count: $\Delta t_{0}$ is
still the finest volume's requirement and the macro cycle still walks it.
On a reach with a 40× length ratio the solver evaluates 2.5× fewer faces
at the same base step. On a nearly uniform mesh there is little to
separate and the bookkeeping is a small net cost, which is why the tier
assignment is cached across cycles and refreshed only when the census
shows the model's stiffness has moved.

### 8.5.7 Second-order reconstruction

`FV_ORDER 2` enables MUSCL reconstruction with the limiter chosen by
`FV_LIMITER`. Two choices in its construction are load-bearing:

- The **free surface** $\eta$ and the **velocity** $v$ are
  reconstructed, not $A$ and $Q$. A lake at rest has $\eta$ constant, so
  every slope is exactly zero and the second-order path degenerates to
  the first-order one — and §8.5.2 already holds exactly. Reconstructing
  depth instead would give every cell on a sloping bed a non-zero slope
  at rest.
- The **bed** is taken from its exact per-cell gradient, not a limited
  one. Depth at a face is then the difference of two separately
  reconstructed quantities, which is what keeps them consistent.

Because the two ends of a cell then see different reconstructed depths,
the hydrostatic flux difference no longer cancels at rest by itself. A
centred bed source restores it:

| | | | |
|---|---|---|---|
| $$S_{c} = \frac{g}{\Delta x}\left\lbrack I_{1}\left( \eta_{i} - z_{i}^{+} \right) - I_{1}\left( \eta_{i} - z_{i}^{-} \right) \right\rbrack$$ | | (8-16) | |

evaluated at the cell's own free surface. Evaluated this way the term
vanishes identically on a flat bed and cancels the flux difference
exactly at rest; evaluated at the reconstructed face depths it would do
the latter but not the former, and would corrupt a dam-break profile.

Linear reconstruction requires a cell small compared with what it is
reconstructing, and the binding scale is the bed. A cell whose ends
differ in elevation by an appreciable fraction of the water depth
cannot carry a linear free surface across itself. Cells failing
$\left| dz \right| < 0.5\,h$ therefore fall back to the first-order
path — which is what makes `FV_ORDER 2` safe to leave on: on an
unresolved long conduit it reproduces the first-order answer rather than
producing a wrong one.

## 8.6 Network coupling

### 8.6.1 Regular junctions and storage units

Nodes are zero-dimensional volumes advanced with the same substep. The
end cell of each conduit exchanges with its node through a ghost state
built from the node head, and that exchange goes through the *same*
Riemann solver as an interior face. Wave reflection off the node,
choking, supercritical approach flow and the surcharge transition are
therefore resolved by the same shock-capturing machinery as the
interior, rather than by a $dQ/dH$ linearization.

Node storage uses SWMM's own convention. A junction, outfall or divider
is a linear reservoir of area $V_{full}/y_{full}$ — that is,
`MIN_SURFAREA` unless a pump wet well overrode it — held fixed for the
run; a storage unit uses its own curve. Matching the engine's
definition exactly is what makes the node ledger conservative: the
volume the solver holds is the same function of depth the mass balance
reports, so no water is stored where continuity cannot see it.

**Mass is conserved exactly at a node. Momentum is intentionally not.**
At a general junction — several pipes at arbitrary angles, differing
sections, a drop manhole — one-dimensional momentum conservation is
ill-posed, because momentum is a vector and the pipe axes do not align.
The standard stagnation-volume closure applies: velocity is zero in the
node, pressure is hydrostatic, and all connected conduits share one
piezometric head. This is the same conceptual model the dynamic wave
solver uses, and like it, it is not an energy balance either — equating
piezometric head discards the incoming velocity head, a dissipative
closure appropriate to a chamber.

**The node's coupling to its faces is semi-implicit.** A junction's
storage area is the `MIN_SURFAREA` floor, which as an effective length
$A_{s}/T$ is a few feet against a conduit $\Delta x$ of several
hundred. Under explicit coupling the manhole, not the pipe, therefore
sets the stable substep for the whole model. `FV_NODE_COUPLING
SEMI_IMPLICIT` (the default) removes that by linearizing each coupling
face's mass flux in the node head, using the characteristic relation
$\left| \partial Q/\partial H \right| = gA/c = \sqrt{g\,A\,T}$ at the
ghost state:

| | | | |
|---|---|---|---|
| $$\Delta H = \frac{\Delta t\left( \sum F + q_{lat} \right)}{A_{s} + \Delta t\sum\sqrt{g\,A\,T}}$$ | | (8-18) | |

The resistance term is always positive — raising the head drives more
out and lets less in, on either side of a face — so the denominator can
only grow and the correction can only damp.

**Conservation survives this by construction, not by care.** The
correction is applied to the face flux itself, which is the single
quantity both the cell update and the node update read, so whatever it
does, the two sides of every face see the same number. Damping the node
*head* directly instead — the obvious approach — would imply a volume
change the incident cells never saw, trading exact mass conservation for
stability. At equilibrium the correction is identically zero, since it
is proportional to the node's net imbalance, so the two couplings agree
on the steady state they reach.

What this removes is the node's *stability* limit, not its *accuracy*
requirement: an under-resolved manhole is still under-resolved however
stable it is. Local time stepping supplies the resolution cheaply, by
giving the node its own fine tier while the conduit cells stay coarse.
With `FV_LTS NO` there is nowhere to put the requirement but the global
step, so the node's Courant limit is honoured there.

### 8.6.2 Virtual junctions

Where a connection really is two collinear pipes of identical section —
the one case where one-dimensional momentum conservation *is* well
posed — the model declares a virtual junction (see the
`[VIRTUAL_JUNCTIONS]` section). Under finite-volume routing these are
consumed at mesh construction: the two conduits' cell chains are
concatenated and the junction becomes an ordinary interior face.

Nothing else is done, and nothing else is needed. Mass and momentum
flux continuity across a virtual junction are then properties of the
scheme rather than a special treatment, and a conduit split by a
virtual junction reproduces the unsplit conduit cell for cell.

### 8.6.3 Outfalls, structures and lateral inflow

Outfalls are stage boundaries: the head computed by the existing
free/normal/fixed/tidal/time-series logic is imposed, and the ghost
cell is built from it.

Pumps, orifices, weirs and outlets are evaluated by their existing
structure equations and applied as source/sink pairs on the two node
volumes. Under `FV_STRUCTURE_COUPLING SUBSTEP` (the default) they are
re-evaluated at the top of every substep, together with the outfall
stage, against the state the solver has actually reached — a routing
step spans many substeps, across which a pump's wet-well depth and a
weir's head difference move while a frozen discharge does not.
`ROUTING_STEP` holds them at their start-of-step values instead.

Control *rules* are not re-evaluated at substep cadence; they run on the
engine's own rule step, which is tuned for routing-step cadence. Only
the head-dependent discharge of the structures those rules set is
refreshed. A device backend cannot call the structure equations across
the plugin boundary and clamps to `ROUTING_STEP`, saying so at open.

Culvert inlet control (§8.6.4) is applied inside the solver, as a cap on
the flux crossing the culvert's upstream face, rather than by rewriting
the link flow afterwards — the node ledger is booked from those fluxes,
so a post-hoc rewrite would leave the reported flow and the continuity
balance describing different runs.

A conduit or outfall flap gate masks the reverse flux at the conduit's
boundary faces. The gate is a check valve, so a masked face behaves as a
wall only while the flux would run the wrong way; closing it mirrors the
interior state across the face, which returns exactly zero mass flux and
leaves the interior its own hydrostatic pressure.

Lateral inflows enter at regular nodes exactly as assembled for any
other routing method. Distributed conduit losses — evaporation and
seepage — enter the cell mass equation as $q_L$ in (8-1).

## 8.7 Reporting

The solver publishes per-link and per-node results at each routing
step, so every existing report, output file and statistic works
unchanged.

Link discharge is the length-weighted **time mean** over the routing
step, not an end-of-step sample: a routing step can span hundreds of
substeps, and a sample aliases badly against them. Link depth and
volume are instantaneous, as under dynamic wave routing. A virtual
junction reports the state of the interior face that replaced it, and
zero volume — it has none by construction.

## 8.8 Scalar transport

When the solver carries advected species, the species flux is the
*same* mass flux the water used, upwinded on the contact speed (8-12):

| | | | |
|---|---|---|---|
| $$F_{\varphi} = F_{mass}\varphi_{L} \ \text{ if } S^{*} \geq 0, \qquad F_{\varphi} = F_{mass}\varphi_{R} \ \text{ if } S^{*} < 0$$ | | (8-17) | |

Flux consistency here is a requirement, not a detail. Computing the
species flux from a separately evaluated velocity — the usual result of
bolting transport onto a hydraulic solver — decouples solute mass from
water mass and produces spurious concentration extrema. Reusing
$F_{mass}$ guarantees exact solute conservation, and that a uniform
concentration field stays uniform under any flow, including reversal
and wetting and drying.

`FV_SCALAR_SCHEME` selects the reconstruction: first-order `UPWIND`,
`MUSCL` (the default), or `QUICKEST_ULTIMATE` (Leonard, 1979, 1991),
which is third order where its two-cell upstream stencil exists and
degrades to `MUSCL` where it does not. The higher-order fluxes are
limited by the flux-corrected transport construction of Zalesak (1979),
which enforces the discrete maximum principle — no new extrema, no
negative concentrations — while preserving conservation exactly.
Clipping the updated concentration instead would enforce the bound but
destroy conservation.

Longitudinal dispersion (`FV_DISPERSION`) is treated implicitly, one
tridiagonal solve per cell chain. The explicit stability limit for a
parabolic term is $\Delta t \le \Delta x^{2}/2D_{L}$, which at fine
$\Delta x$ is far more restrictive than the Courant condition; the
implicit treatment removes it entirely.

## 8.9 Options

All knobs are `[OPTIONS]` keys. They are accepted and inert under any
other routing model, so switching `FLOW_ROUTING` never invalidates a
file.

| Key | Default | Meaning |
|---|---|---|
| `FV_CELL_LENGTH` | 0 | Target $\Delta x$ in project length units. 0 means no length target; each conduit gets `FV_MIN_CELLS` cells. |
| `FV_MIN_CELLS` | 4 | Floor on cells per conduit. Applies with or without a `FV_CELL_LENGTH` target. |
| `FV_CFL` | 0.5 | Courant number $\alpha$ in (8-14). |
| `FV_RIEMANN` | `HLLC` | Species flux: `HLLC` resolves the contact wave, `HLL` averages it. No effect on hydraulics. |
| `FV_ORDER` | 1 | 1 or 2. Second order is MUSCL on $(\eta, v)$ with the guard of §8.5.6. |
| `FV_LIMITER` | `MINMOD` | `MINMOD`, `VANLEER` or `SUPERBEE`, with `FV_ORDER 2`. |
| `FV_SCALAR_SCHEME` | `MUSCL` | `UPWIND`, `MUSCL` or `QUICKEST_ULTIMATE`. |
| `FV_TIME_INTEGRATION` | `EULER` | `EULER` or `RK2` (Heun, SSP). `RK2` disables local time stepping. |
| `FV_SLOT_CELERITY` | 100 | Pressurized wave celerity in project length units per second; sets the slot width via (8-5). |
| `FV_DISPERSION` | 0 | Longitudinal dispersion coefficient. 0 disables the parabolic term. Accepted but **inert** until finite-volume transport is connected (§8.8); a non-zero value warns at open. |
| `FV_STRUCTURE_COUPLING` | `SUBSTEP` | Cadence at which structure flows and outfall stages are refreshed: every substep, or once per routing step. A device backend clamps to `ROUTING_STEP`. |
| `FV_NODE_COUPLING` | `SEMI_IMPLICIT` | `EXPLICIT` freezes face fluxes across the node update; `SEMI_IMPLICIT` linearizes them in the node head (§8.6.1). |
| `FV_COMPACTION` | `YES` | Skip dry, inactive parts of the network. Results-transparent. |
| `FV_LTS` | `YES` | Local time stepping (§8.5.6). `NO` forces one global substep size. |
| `FV_LTS_MAX_TIERS` | 6 | Cap on the tier spread; 6 allows 64×. |
| `FV_CFL_CENSUS_INTERVAL` | 1 | Substeps between full Courant censuses. 1 recomputes every substep. |
| `FV_BACKEND` | `AUTO` | `CPU`, `AUTO`, `OMP`, `CUDA`, `HIP` or `SYCL`. |
| `FV_MIN_PARALLEL_CELLS` | 20000 | Mesh size below which `AUTO` stays on the CPU. |

## 8.10 Choosing between dynamic wave and finite volume

The following are measured on the EPA reference site drainage model
(`Example1.inp`, 30 h, 5 s routing step), comparing each finite-volume
configuration against the dynamic wave run of the same file.

| | Dynamic wave | FV, 1 cell | FV, default (4 cells) | FV, $\Delta x$ = 50 ft | FV, $\Delta x$ = 20 ft |
|---|---|---|---|---|---|
| Routing continuity error | 0.026 % | 0.000 % | 0.000 % | 0.000 % | 0.000 % |
| Mean absolute peak-flow deviation | — | 37.1 % | 15.3 % | 12.8 % | 7.2 % |
| Wall-clock, relative | 1× | ~7× | ~15× | ~12× | ~34× |

The same comparison across network size, on uniform and graded reaches
of 50 / 500 / 2000 conduits, at the default mesh:

| | 50 | 500 | 2000 |
|---|---|---|---|
| uniform reach, × dynamic wave | 117 | 252 | 189 |
| graded reach, × dynamic wave | 39 | 69 | 49 |
| routing continuity, finite volume | −0.000 % | −0.000 % | −0.000 % |
| routing continuity, dynamic wave | −0.16 … −0.50 % | 0.09 / −0.19 % | 0.09 / −0.19 % |

Two readings. **The ratio does not improve with network size** — it is
flat to erratic, so no argument that the cost amortizes on larger models
survives this table. And **the finite-volume solver closes on the
dynamic wave solver exactly where the latter struggles**: the graded
ratios are three to four times better, not because the explicit solver
got faster (its times are unchanged between the two) but because the
implicit solver's own step collapses there, from 5.00 s to 1.14 s. Steep,
graded, stiff networks are where the trade is most favourable.

Three readings follow, and all three are honest.

**Conservation is delivered, and is independent of resolution.** The
finite-volume solver closes continuity exactly on this model where the
implicit solver does not. That is the property the conservation form
guarantees.

**Accuracy requires a resolved mesh.** The convergence above is clean
and monotone, which is what a consistent discretization must show — but
an unresolved mesh is not a drop-in substitute for dynamic wave
analysis. The mechanism is §8.3's artificial bed step, not numerical
diffusion, so second-order reconstruction does not rescue it.

**It still costs several times more.** Even at one cell per conduit —
the same element count the dynamic wave solver carries — the explicit
method runs about seven times its wall-clock on this model, and the
default mesh roughly doubles that again. Choose it for what it does, not
for speed.

Note also that the deviation column is a *consistency* check against the
dynamic wave solver, not a measure of error. Dynamic wave routing is not
the reference truth here, and part of the 37 % at one cell is its own
departure from the correct answer. Accuracy is established against
closed-form solutions — Ritter, Stoker, and the still-water property —
not against another numerical method.

Practical guidance:

- **Use dynamic wave analysis** for routine design storms, continuous
  simulation, and planning work. It is faster, it is the validated
  default, and the phenomena the finite-volume method resolves better
  are not what drives those results.
- **Use finite-volume routing** when volume conservation must be exact,
  when a pressurization front's arrival time matters, when steep sewers
  cross the critical condition, or when in-conduit profiles are the
  subject of the study. Set `FV_CELL_LENGTH`.
- If the goal is simply better continuity and stability under dynamic
  wave routing, `SURCHARGE_METHOD SLOT` with a tightened
  `HEAD_TOLERANCE` addresses most of it at no cost, and virtual
  junctions remove the storage error at genuine grade breaks.

## 8.11 Limitations

- **Sub-atmospheric pressure cannot be represented.** The slot closure
  is monotone — head below the crown means a free surface — so negative
  pipe pressures have no representation. Air-phase effects are likewise
  out of scope. This is the same fidelity limit the dynamic wave
  solver's slot carries.
- **Junction momentum is not conserved** (§8.6.1), by choice.
- **A conduit loop closed entirely by virtual junctions cannot be
  meshed**, because such a cycle has no boundary. Ordinary junctions
  break the cycle and are accepted.
- **Every conduit must carry real cross-section geometry.** A control
  volume requires a section; a conduit whose full depth or area is zero
  is rejected at initialization rather than silently routed.
- **Hot start files are not interchangeable between routing models.**
  A dynamic wave hot start projects onto the finite-volume mesh with a
  warning; the reverse discards in-conduit structure.

## 8.12 References for this chapter

Audusse, E., Bouchut, F., Bristeau, M.-O., Klein, R., and Perthame, B.
(2004). "A fast and stable well-balanced scheme with hydrostatic
reconstruction for shallow water flows." *SIAM Journal on Scientific
Computing*, 25(6), 2050–2065.

Harten, A., Lax, P. D., and van Leer, B. (1983). "On upstream
differencing and Godunov-type schemes for hyperbolic conservation
laws." *SIAM Review*, 25(1), 35–61.

Hou, T. Y., and LeFloch, P. G. (1994). "Why nonconservative schemes
converge to wrong solutions: error analysis." *Mathematics of
Computation*, 62(206), 497–530.

Leonard, B. P. (1979). "A stable and accurate convective modelling
procedure based on quadratic upstream interpolation." *Computer Methods
in Applied Mechanics and Engineering*, 19(1), 59–98.

Leonard, B. P. (1991). "The ULTIMATE conservative difference scheme
applied to unsteady one-dimensional advection." *Computer Methods in
Applied Mechanics and Engineering*, 88(1), 17–74.

Toro, E. F. (2001). *Shock-Capturing Methods for Free-Surface Shallow
Flows*. Wiley, Chichester.

Zalesak, S. T. (1979). "Fully multidimensional flux-corrected transport
algorithms for fluids." *Journal of Computational Physics*, 31(3),
335–362.
