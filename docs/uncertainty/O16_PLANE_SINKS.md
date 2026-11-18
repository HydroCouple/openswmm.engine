# O-16 — plane sinks below the invert (diagnosis, 2026-11-18)

Diagnosis only; no engine change. Code at 3a41aae8 (O-15), Release `darwin-v2-rel`, per-member OFF (the shipped path).
Data: 1D smoke `bellinge_demo/demo_o16.*` (= `ph_smoke1d.inp` run to 10:00, 80 s) with the O-14 plane trace for 22 nodes
(`SWMM6/data/o16_diag/plane_trace.csv`); coupled OFF `demo_off.uncertainty.csv`; MC `mc_v3/` (10 members, Hazen q05/q50/q95,
script `o16_diag/cmp_off.py`). One rain plane (two NORMAL CV 0.20 gages, COHERENCE FULL), M = 50, member level =
main run + c_i·Psi with c_i = probit((k+½)/50) ∈ [−2.33, 2.33]; the 90 % band is ±1.645·Psi before the invert clamp.

## 1. Premise check

* **Psi ≥ 0 at every traced node and step** (min 0.000 over 145,761 rows). The source (runoff × spread × E) is ≥ 0 and
  S/dt + J is an M-matrix, so the plane is nonnegative by construction: no plane line ever runs backwards *per unit
  coefficient*. A member "below the invert" is the mirror image (c_i < 0) of a member equally far ABOVE the main run.
  The low side is clamped and hidden; the high side is not: coupled OFF prints G71O020 q95 = invert + **380 ft** at 08:00,
  G70F11P q95 = invert + 15.6 ft at 05:40 (MC 2.11).
* **G72R530 / G72R540 / G72R560 / G72R570 / G80F370 are not plane sinks.** OFF smoke: Psi ≤ 0.11 ft, never below the
  invert. The ON log already says so: their counts (19,360 / 16,775 / 14,890 / 11,265 / 9,124) are all in the
  "correction-only" line (shared-plane level ≥ −0.05 ft) — the per-member correction, O-9's "second mechanism behind
  conduit lines". G73F362: OFF plane −0.37 ft deepest (268 steps); 13,295 of its 14,704 ON counts are correction-only.
  Out of scope here.
* The OFF plane's real sinks (smoke, member-steps with c_min·Psi below the invert by > 0.05 ft, deepest, hours):
  G71O020 2,895 / −89.0 ft / 2.8 h; G70F11P 1,493 / −52.6 ft / 1.9 h; G72F646 1,011 / −11.5 ft / 0.8 h (cluster
  G72F645/647/648 alike); G71F041 307 / −22.0 ft; G71F12P 3,120 / −1.2 ft / 3.2 h. (ON coupled: G71O020 −205.8 ft.)

## 2. Evidence (top 3; heights in ft above invert)

| node | links (inp) | S (ft²) | trace | MC q05/q50/q95 | OFF q05/q50/q95 |
|---|---|---|---|---|---|
| G70F11P junction wet well, inv 10.92 m, no subcatchment | in: 2 × 0.15 m pipes from G70F301; out: pumps p1, p2 PUMP4 **flat curves** (0.021 / 0.011 m³/s at all depths), y_on 0.6/0.7 m, y_off 0.01 m | 15.9 at the pump level, 1.167 surcharged | pump edges `d = 0` (both pumps); 05:00–05:41 Psi 3.7 → 8.4 at constant main depth 2.02 ft; 09:41 Psi 16, 09:59 23.5 | 05:20 2.03/2.06/2.11 | 0.00/2.25/12.54 |
| | | | | 05:40 2.03/2.06/2.11 | 0.00/2.30/15.62 |
| G71O020 junction, inv 11.13 m, no subcatchment | in: side orifice G71F12Pw1 (crest 0.321 m) from pump well G71F12P; out: one 0.25 m pipe | **1.167 (MIN_SURFAREA, main run dry)** | orifice q* = 0, secant d 0.87–0.97; outlet = dry-pipe secant d = Qn(y_full)/σ falling 0.30 → 0.0045 as σ = 1.645 Psi grows; Psi 0.53 → 11.2 → 31.0 in 120 s (22,493–22,617 s); after wetting S → 36.5, O-14 carry ÷31 | 06:02 0/0/0; 07:41 0.04/0.04/0.04 | 06:02 0/0.03/1.43; 07:41 0/0.06/1.07; 08:01 0/7.29/**380** |
| G72F646 junction, inv 28.64 m | in: 2 × 0.104 m; out: 0.181 m | 11.07 → **1.167** at 22,576 s (crown entry) | Psi 0.92 → 4.14 in one step (×11.07/1.167, the O-14 volume carry); c_min·Psi = −9.6 ft at depth 0.67 | 06:21 1.17/2.39/4.47; 06:41 0.61/1.08/1.64 | 0.00/2.67/9.36; 0.00/1.16/2.42 |

Upstream of G71O020: G71F12P (pumps PUMP4 flat, `d = 0`; only plane outlet = the orifice secant) — MC 07:41
0.36/0.48/0.62, OFF 0.00/0.58/1.60: its over-wide σ is what opens the orifice secant (crest 1.05 ft above invert;
main 0.57 + σ ≥ crest while every MC member stays below it).

## 3. Mechanisms (ranked by evidence)

**A. Missing pump outlet (G70F11P, G71F12P; feeds G71O020).** `computeRom1dFlowSource` gives pumps `d = |dqdh|` and
excludes them from the structure secant (`is_struct` drops PUMP). A PUMP4 curve flat in depth has dqdh = 0, so the pumped
well has no plane outlet: every member's extra inflow accumulates in S ≈ 16 ft². The MC shows the opposite — the well is
held at the start level (spread 0.08 ft) and the pump passes the extra water by running longer. The −50 ft low side is
the mirror; the source-side reading (deficit booked against a dry well) does not apply: G70F11P is wet (2 ft) throughout.

**B. Reversed one-way line at q* = 0 (G71O020; O-9's mechanism).** The orifice and the dry outlet pipe carry no main-run
flow; both get a SYMMETRIC secant over ±σ (H10 option A). For c_i < 0 the member line is q* + d·c_i·Psi_up = −d·|c_i|·Psi_up
< 0: water leaves a dry node through a closed orifice. The secant was built for the upper side (a member above the crest
spills); applied to the lower side it is a reversed line. Amplified by (i) S = MIN_SURFAREA at a dry node and (ii) the
dry-pipe outlet d ∝ 1/σ once σ exceeds the pipe (same feedback O-14 rejected for S).

**C. Sign-blind storage at crown entry (G72F646 cluster; O-14 open item b).** One S serves both sides. Entering
surcharge, S drops ×9.5; the carried volume is right for the members above the crown and wrong for those below, whose
deficit is divided by the surcharged S (MC q05 1.17 ft vs plane −6 ft unclamped).

**Rejected by the data.** (4) source floor δq_i ≥ −q_in,i: dq = 0 at all three nodes (no subcatchments), and for direct
sources |c|·CV·E = 2.33·0.20·E < 1 while E < 2.15 (max seen 1.47), so it never binds. (3) pin to the crest: G71O020's MC is
dry (0.04 ft), not at a crest; pinning drops water. (1) per-member floor Q = max(0, ·): not representable in a shared
linear plane; its output form is the invert clamp, which is what hides the defect.

## 4. Recommended law (no knob): sign-split planes with one-sided secants

Every additive plane p becomes two planes carried by κ⁺ = max(κ, 0) and κ⁻ = min(κ, 0) (member deviation
κ⁺Ψ⁺ + κ⁻Ψ⁻). Each half is linear and stable (same AdditiveSystem, M-matrix), assembled with the secant of the member
rating over ITS half of the member range, σ± = 1.645 RMS of that half:
* structures and pumps (pumps via `StructureSolver::pumpFlowAtDepth`, the O-9 (b1) table, which honours y_on/y_off):
  d⁺ = max(tangent, (Q(h*+σ⁺) − Q*)/σ⁺), d⁻ = max(tangent, (Q* − Q(h*−σ⁻))/σ⁻). Ratings are ≥ 0, so the low member's
  line q* + d⁻·z is ≥ 0 at z = −σ⁻: **the zero-flow floor in the line's own direction, at the member range** (candidate 2,
  made representable). Closed structure / dry pipe (q* = 0): d⁻ = 0 — no reversed line (B). Pump below capacity:
  d⁺ = (Q_cap − Q*)/σ⁺ — the pump passes extra inflow (A). Pump at capacity: d⁺ = 0 (wet members do fill), d⁻ > 0 once
  h* − σ⁻ reaches y_off — the "about to stop" counterpart of the "about to start" secant.
* dry pipes: d⁺ = Qn(y + σ⁺)/σ⁺ (today's), d⁻ = 0.
* flowing conduits: tangent ju/jd unchanged, identical in both halves.
* stage 2, only after stage 1 is gated: S⁺/S⁻ = one-sided secants of V(h) (fixes C; the symmetric S secant was rejected in
  O-14 for its 1/σ feedback — the lower half is bounded by V(h*), the upper half by the surcharge area).

Where both halves' edge lists are equal (no threshold, no dry pipe, no pump in the active set), build one system and set
Ψ⁻ = Ψ⁺: κ⁺Ψ + κ⁻Ψ = κΨ exactly (one term is 0) — **bit-identical** to today. The Manning flow response
(`solveFlowResponse`, H18 diag) keeps today's edge list (pump/half secants are rain-channel only, like `rom1d_fs_dry_edges_`).

**Implementation.** `computeRom1dFlowSource`: build a second rain-channel edge list with (d⁺, d⁻) per one-way edge,
pumps included (do not touch `rom1d_fs_edges_`); `memberRange` → per half. `stepRom1dAdditiveChannel`: split kap into
κ±, build `rom1d_add_sys_` (+) and a second `AdditiveSystem` (−) only when the lists differ, carry both Ψ± through the O-14
S_prev rescale, hand 2P planes to `setAdditivePlanes` (no SpectralROM1D change). `stepRom1dStorageMembers` consumes the
same (q, kap) pairs — check it accepts the split (its u_i = Σκ·Ψ is unchanged in form).

## 5. Expected effect

* G70F11P before the burst: Ψ⁺ bounded by the spare pump capacity (Q_cap − Q* ≈ 0.7–1.0 cfs vs per-unit extra inflow
  ≈ 0.05 cfs) → band of order the MC's 0.08 ft instead of ±14 ft; low side no longer below the invert.
* G71F12P band narrows toward MC (σ 1.16 → ~0.3 ft) → the orifice secant stays shut while the main run is 0.5 ft under
  its crest → G71O020 Ψ⁺ ≈ 0 when the MC is dry; Ψ⁻ = 0 always (d⁻ orifice = 0). The 380 ft q95 and the −206 ft member go.
* G72F646 unchanged in stage 1 (mechanism C); stage 2 targets it.
* SR-5 / SR-7 / CL-1e: 5 STORAGE nodes (FUNCTIONAL 20,000 ft² or small), 5 circular conduits, no pump/weir/orifice/outlet.
  Only the dry-pipe edges of the first minutes differ (pipes start empty) → expect late-window numbers identical at the
  printed digit; not bit-identical (spin-up). Storage gate (basin + transverse weir): B moves — upper secant over
  [h*, h*+σ] is up to 2× today's symmetric one (B's upper edge was 1.5 cm short: may improve or overshoot); lower side
  d⁻ = 0 below the crest is the MC's behaviour. Structures gate (Example 8, Manning only, no additive plane):
  bit-identical. Cost: one more sparse factorisation per step only on steps where the halves differ.

## 6. Test that pins it

`AdditiveSignSplit.ThresholdOutletsDoNotDrainDryMembers` (test_additive_channel): J0 (rain source) → side orifice,
crest 0.3 ft above the main-run level → J1 junction (no storage curve) → 0.5 ft pipe → O1; J0 also → pumped well W
(PUMP4 flat, y_on above, y_off below the main level) → O2. 21 deterministic runs at rain × (1 + 0.2 c_k) as MC.
Assert: (a) unclamped member level at J1 ≥ invert − 1e-9 for every c_i < 0 (mutation = symmetric secant: J1 below the
invert by > 1 ft); (b) plane/MC width ratio in [0.5, 2] at W while the pump is below capacity (mutation = pump d = 0:
ratio > 5); (c) `SignSplitIsBitIdenticalWithoutThresholds`: conduit-only flowing chain, EXPECT_EQ member levels vs the
single plane.

## 7. Gates to run (Release, in this order)

additive_channel, spectral_rom1d, member_ports, port_graph_replay; storage gate A/B/R; SR-5, SR-7, CL-1e; structures
(Example 8, expect bit-identical); Bellinge OFF smoke (plane trace on the nodes above + any-node counter with ON) and
coupled OFF vs mc_v3 — hold 1D all 0.949/1.261 and storage 0.811/0.948; watch G70F11P, G71F12P, G71O020, G72F646.
