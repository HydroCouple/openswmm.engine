# Full-order additive channel (H19) — design note

Status: design fixed 2026-10-16 (owner chose option 2 after H18 A.4b,
VALIDATION.md "H18" §10–§10c); the default (and only) additive path since A.5,
2026-10-16. It replaces the modal forcing of every additive
uncertainty: soft rain (gage planes and CORR_LEN fields), the built-in runoff
multiplier, and registered `FORCING_VECTOR` sources such as `INFLOW`. The
Manning channel (modal, H17 target + H18 rates) is unchanged.

## 1. Why

Two measured failures of the modal additive forcing:

1. **The downstream transfer.** A member with more rain carries more flow in
   every pipe below the extra water. H18's stable symmetric operator drops that
   transfer (SR-5 J5 at 0.3–2 % of the MC). Relaxing toward the steady
   flow-response target restores it, but delivers it at once (J5 3–25× wide on a
   storage-dominated network). Only dynamics that contain the one-way transfer
   are right both before and after the flows settle, and projecting a one-way
   operator onto a few modes was unstable in October.
2. **The source.** Soft rain was driven by the deterministic head rate dh/dt
   times the relative spread. Under steady rain in steady flow dh/dt = 0, so no
   band forms although the rain uncertainty changes every flow.

## 2. The model

For each additive source p the channel carries one full-order head deviation
field Ψ_p (all active nodes, no mode truncation), shared by all members:

    S dΨ_p/dt = −J Ψ_p + δq_p

- **S:** node storage (storage-curve area + half the top width of attached
  conduits, floored at MIN_SURFAREA; the dynamic-wave rule, from published
  state).
- **J:** the linearized flow Jacobian of the flow-response sweep: per conduit
  δQ = Ju·Ψ_up + Jd·Ψ_dn, and per structure δQ = |dQ/dH|·Ψ_up, oriented by
  flow. This includes the one-way transfer, the Froude relay and the full-pipe
  bound exactly as the Manning target uses them. Outfalls hold zero deviation.
  A node overflowing at its rim is pinned at zero (the extra water spills).
- **δq_p:** the source's extra inflow per unit member coefficient (§4).

Member i's additive deviation is Σ_p κ_ip·Ψ_p, added to its reconstructed head
next to the Manning deviation:
- comonotone soft rain: κ = c_i (plane A) and c_i^B (plane B);
- CORR_LEN reduced: one Ψ_m per spatial mode, κ = a_im;
- CORR_LEN materialized: one Ψ per member, κ = identity;
- runoff multiplier: κ = scale_i − 1;
- forcing vectors: κ = θ_i − 1.

**Time step.** Backward Euler, (S/dt + J)·Ψ_new = (S/dt)·Ψ_old + δq. The
matrix is a strictly column-diagonally-dominant M-matrix: each column's
off-diagonal sum is exactly its diagonal minus S/dt, and the column sums are
volume conservation. So it is nonsingular and the step is unconditionally
stable. Since 2026-10-24 it is solved directly: a sparse LU factorization
without pivoting (elimination keeps a column-dominant M-matrix one, so every
pivot stays positive), in a minimum-degree order computed on the union of
every link pair seen so far and recomputed only when a new pair appears. The
matrix is the same for every plane, so it is factored once per step and each
plane costs one forward and one backward pass. On a dendritic network the
order eliminates leaves first with no fill; each loop adds a few entries
(Bellinge: 1,044 links, 1,020 nodes, 25 loops). Pinned nodes lose their row
and column. Before, Gauss–Seidel (alternating sweeps, tolerance 1e-10, at most
200 sweeps) averaged 122 sweeps per step on Bellinge's real storm and hit the
cap on more than a tenth of steps; the Release step went from 8.45 to about
7.1 ms with identical gates.

**Limits of the linear model (documented, not fixed):**
- Members do not scale J by 1/mm, so the roughness × rain interaction, a
  second-order effect, is dropped.
- Conduits with no flow carry no conductance, so a still pool simply stores
  its extra inflow. That is the physics of a pool without outflow.

## 3. What it costs

One factorization per routing step, then one forward and one backward pass per
plane, O(nodes + fill) each (microseconds on Bellinge).
- Gage soft rain: two planes at most.
- CORR_LEN reduced: K_s planes.
- CORR_LEN materialized: M planes.
- Reconstruction adds Σ_p κ_ip Ψ_p at report boundaries: O(M·P·n).

## 4. Sources (extra inflow per unit coefficient, cfs)

- **Soft rain, plane A or B:** Σ over the node's contributing subcatchments of
  runoff_s × relative spread of its gage × rain→runoff elasticity E_s (SR-6),
  restricted to that plane's family. The relative spread is CV, or SD or
  HALFRANGE over the gage rainfall, as today. No area weighting is needed,
  because each subcatchment contributes its own runoff.
- **CORR_LEN:** the plane-A source multiplied pointwise by the spatial field (ψ_m
  or W_i).
- **Runoff multiplier:** the node's subcatchment runoff.
- **Forcing vectors:** the node's lateral inflow, so "INFLOW 0.30" means
  inflow 30 % larger. Before, it meant head changes 30 % stronger.

## 5. What the ROM does

`SpectralROM1D` still generates the per-member
coefficients (`softCoefficients(family)` returns c_i or c_i^B). It applies no
additive modal forcing (the modal path was deleted on 2026-10-19), and its
reconstruction (`computeQuantiles`, `reconstructHead`) adds Σ_p κ_ip Ψ_p from
`setAdditivePlanes(P, Ψ, κ)`. The Manning channel, the phase coordinate and the
flags are unchanged.

## 6. Validation (gate)

| Gate | Requirement |
|---|---|
| SR-5, CL-1e (DW and FV) | calibrated |
| **New:** steady rain on steady flow (SR-7) | calibrated; the old channel gives no band |
| Every Manning gate (chains DW/FV, Example 8, Bellinge MC) | unchanged (no additive source) |
| Unit tests | steady state = flow-response sweep with accumulated throughflow; early response local (no instant transfer); volume conserved; zero coefficient exact |

After this gate passes, H18 A.5 resumes: H18 as the default, then K1d, the
advective hook and the old modal additive forcing are deleted.

## 7. Storage nodes: each member's own level (2026-10-24)

**Status: off by default** (`rom1dStorageMembersEnabled()`, an internal
measurement hook). It is calibrated on the threshold gate below. On Bellinge's
real storm, each problem found and fixed so far exposed the next one downstream.

Fixed (2026-10-31 to 11-01):
- **Report-lagged secants.** The secants (structures, dry pipes) took their
  range from the last report's band, their own output, and flipped wide and
  narrow every report. They now use 1.645 × the RMS of the members' rain-channel
  deviation at the node, from the previous step.
- **Drowned pipes.** A pipe whose end is drowned is two-way with no
  threshold, so it keeps the planes' straight line. The friction square root
  over a near-zero head difference made a stiff pair (G71F320/G71F68Y).
- **Outlets closed in the main run.** Weirs, orifices, outlets and pumps
  enter a per-member node's ratings from the link table even when the main
  run's flow is zero.
- **Pumps.** A depth-rated pump (PUMP2/PUMP4 with startup or shutoff depths)
  is rated per member, relative to the main run's actual flow. An empty
  member pumps only what arrives. A pump switched by control rules cannot be
  followed per member, so its node stays in the planes (G71F68Y).
- **Raised-end basins.** A storage node with a pipe whose far end is raised
  is admitted from the start. That pipe is one two-way full-pipe-friction link
  between the two ends' controlling levels, owned by the basin. The node at
  the far end is admitted too and takes the basin's exchange as inflow.
- **Downstream levels.** A rating reads a downstream non-per-member node's
  plane deviation only, not its correction. The correction is fed by the
  node's own outflow, and reading it closed a lagged loop (G72UD11 reached
  550 ft).
- **Level bounds.** A member's level stays between the node's invert and its
  rim plus surcharge.

Still open: G72R83B now holds its drier members within 0.1 ft (Monte Carlo
0.06 m), but late in the storm its wetter members still rise. G72UD11's
members spread ±1.8 m, symmetric, against 1.2 m one-sided in the Monte Carlo,
which caps them through its outlet pipe to G72R580. That pipe is still a
straight line from the main run. Coupled manholes (step 3) are implemented
behind the same hook and have not been measured on their own.

**Why.** A plane gives member i the extra head κ_i·Ψ everywhere, with one
straight-line slope per outlet taken at the main run's state. Behind an outlet
that starts at a level that slope is zero or nearly zero, so a member's extra
rain piles up for the whole storm:
- a weir or orifice whose crest the main run is below, or just above;
- a flow brake on the flat part of its rating;
- a pipe that ends above the next manhole's water, whose flow is set by the
  few centimetres over its end and not by the drop to the next manhole.

Measured on Bellinge's real storm: the G72R basins carried 1–3 m bands against
5–13 mm in the Monte Carlo.

**What.** Every active STORAGE node carries each member's own level y_i
(relative to the main run), stepped by backward Euler on its own balance and
solved by bisection (the residual is monotone):

    S (y_i' − y_i)/dt = ΔI_i(y_i') − Σ_out ΔO_link,i(y_i')

- **Inflow ΔI:** the plane sources at the node times κ_i, plus each inflow
  link's extra flow from the member's upstream deviation (plane plus
  correction). A storage node upstream hands over its members' actual extra
  outflow of the last step.
- **Structures (weir, orifice, outlet):** a table of the engine's own
  structure flow at 17 shifted levels, clustered near the main run's level
  (y = Y·t|t|), since ratings curve most just above a crest. Linear
  interpolation in between.
- **Conduits:** Q = q0 · K(y_up)/K(y0) · sqrt(dh/dh0), conveyance at the
  member's upstream depth times friction over the head above the downstream
  control. The control is the higher of the next node's water and the pipe's
  downstream end, so a raised end cuts the flow off. At the main run both slopes
  equal the plane's. A supercritical pipe (relay off) keeps the conveyance
  factor only.
- **Pumps:** their linear slope.

**Downstream.** What a member's storage nodes actually release, minus what the
planes delivered with their straight lines, is the source of one correction
field per member: the same matrix, storage nodes held, a second direct solve.
At a storage node the correction is set so the member reads its own level. The
ROM adds it through `setMemberAdditive` (members × nodes).

**Approximations.**
- The storage area is the main run's.
- A storage node's level difference does not travel upstream as backwater.
- A member's downstream head in the conduit rating lags one step.
- The Manning channel's deviation is not part of the storage node's level.

**Dry pipes (rain channel only).** A conduit the main run leaves dry has zero
capacity and zero slope at zero depth. Without a conductance, a member's water
spilled into it would sit at its upstream end for good. It gets the secant of
its normal-flow rating over the upstream node's member range:
d = Q_normal(depth + σ)/σ, one-way downhill. It is not added to the Manning flow
response: there it moved the Example 8 Manning gate from 0.825 to 0.747.

**Validation.** `regression_storage_threshold_coverage`: a catchment drains to
a basin with a transverse weir, then three manholes and an outfall. 21 Monte
Carlo members are compared with the sidecar, with and without per-member levels
(coverage / width ratio).

| Scenario | Basin, per-member | Basin, planes only | Manhole below the weir |
|---|---|---|---|
| A: main run spills | 0.892 / 0.966, calibrated | 0.719 / 0.080 | 0.718 / 0.505 |
| B: main run 3 cm below the crest | 0.730 / 0.966, ranking | 0.774 / 0.627 | 0.909 / 1.41 (840× before the dry-pipe secant) |

In B the band has the right lopsided shape: −0.80/+0.11 ft around the median,
against −0.82/+0.13 ft in the Monte Carlo. The wetter members bunch just over
the crest, and the band's upper edge sits 1.5 cm short of that bunch. Below a
stopped weir the pipe empties. That manhole is not a storage node, so its band
stays a ranking.

Every Manning gate is unchanged at the printed digit. The rain gates are within
2 %: SR-5 0.921/1.028, CL-1e 0.941/1.031, SR-7 0.921/0.987.
