# The Preissmann Slot in OpenSWMM: Finite Volume vs Dynamic Wave

**Date:** 2026-08-24
**Prompted by:** TwinOaks transmission-main model (9-ft circular main, `FLOW_ROUTING FV`,
`FV_SLOT_CELERITY 50`, `SURCHARGE_METHOD EXTRAN`) — the observation that a significant
portion of the FV run's stored volume sits in the Preissmann slot.

Every claim below carries a `file:line` reference into the engine sources at the time of
writing (branch `swmm6_rel`).

---

## 1. When the slot engages

### 1.1 Finite volume — the slot is the geometry, not a mode

The FV router has **no trigger and no regime switch**. The slot is folded permanently into
the cross-section closure A(h), T(h), R(h), so every cell evaluates one continuous geometry
from dry bed to full pressurization; a "transition" is nothing more than a cell's depth
crossing the crown (`docs/manuals/reference/hydraulics/sections/Chapter8-FiniteVolume.md`
§8.4; `src/engine/hydraulics/fv/FvKernels.hpp:159-178`).

Concretely, slot area starts accumulating at

```
h > y_crown = 0.985257 · y_full        (SLOT_CROWN_CUTOFF, Constants.hpp:140,
                                        NetworkMeshBuilder.cpp:119)
```

through a C¹ smoothstep ramp, and the slot is fully open at `h ≥ y_full`:

```cpp
// FvKernels.hpp:159-167
if (h >= g.y_full) return g.a_crown + g.t_slot * (h - g.y_full);
const double s = (h - g.y_crown) / band;
return ax + g.t_slot * band * slotRampIntegral(s);   // ramp φ(s) = s²(3−2s)
```

Open sections (RECT_OPEN etc.) get vertical-wall extension instead (`t_slot = w_max`,
no taper — `NetworkMeshBuilder.cpp:108-113`).

### 1.2 Dynamic wave — three surcharge methods, only one has a slot by default

`SURCHARGE_METHOD` defaults to **EXTRAN** (`core/SimulationOptions.hpp:366`), which is what
the TwinOaks deck uses. Under EXTRAN there is **no slot at all**:

- `getSlotWidth()` returns 0 unconditionally (`hydraulics/DynamicWave.cpp:150-152`);
- conduit depth is clamped to `y_full`, width capped at `0.96·y_full`
  (`EXTRAN_CROWN_CUTOFF`, `DynamicWave.cpp:1549-1552, 1567-1570`);
- surcharge is handled at the **node** by point iteration on head,
  `Δy = corr·ΔQ / Σ(dQ/dH)` with an `exp(−15f)` crown blend (Manual Eq. 3-28;
  `DynamicWave.cpp:3456-3485`);
- a node counts as surcharged when its head exceeds the crown of its highest connecting
  link (`DynamicWave.cpp:3306-3320`); a conduit counts as full when both ends reach
  `y_full` (`DynamicWave.cpp:2170`).

`SURCHARGE_METHOD SLOT` engages the **Sjöberg slot**: width from `0.985257·y_full`
(the same cutoff FV uses), slot **area** only above `y_full`
(`DynamicWave.cpp:125-153, 2136-2164`; bit-parity with legacy `dwflow.c:652-665`).

`SURCHARGE_METHOD DYNAMIC_SLOT` is the path-dependent DPS variant (Sharior et al. 2023),
target celerity 25 ft/s by default (`DynamicWave.cpp:196-274`, `DynamicWave.hpp:83-88`).

**So a comparison of "FV vs DW" on the same TwinOaks deck compares a slot solver against a
no-slot solver.** DW/EXTRAN cannot store any water above the pipe crown inside a conduit;
FV always can.

---

## 2. Slot widths and tapering

### 2.1 The formulas

| Engine | Width above crown | Taper |
|---|---|---|
| FV | `T = g·A_full/c²`, **constant**, capped at `0.05·W_max` (`NetworkMeshBuilder.cpp:114-127`) | C¹ smoothstep only across `[0.9853·y_full, y_full]` — a continuity device for the Riemann wave speeds, not storage relief (Manual §8.4.2, Eqs. 8-6/8-7) |
| DW SLOT | `w = 0.5423·W_max·exp(−(y/y_full)^2.4)`, floored at `0.01·W_max` for `y/y_full > 1.78` (`DynamicWave.cpp:125-153`) | the Sjöberg exponential **is** the taper — width *decays* as surcharge deepens: ≈20.7 %·W_max at the crown → 1 %·W_max deep |
| DW EXTRAN | — (no slot) | — |
| DW DPS | dynamic `T_s = (g·A_C/c_pT²)·P²`, path-dependent | relaxes over time |

The FV slot is therefore **not** the Sjöberg slot: constant width forever above the crown
versus a width that shrinks 20× between the crown and 1.78·y_full.

### 2.2 Worked numbers — the TwinOaks 9-ft main

`A_full = π·4.5² = 63.62 ft²`, `W_max = 9 ft`, cap `= 0.05·9 = 0.45 ft`.

| Configuration | Slot width | Slot storage per 1000 ft at 10 ft surcharge head | % of pipe volume |
|---|---|---|---|
| FV, c = 50 (deck value) | `32.2·63.62/2500 = 0.82 ft` → **capped at 0.45 ft** (effective c ≈ 67.5 ft/s) | 4,500 ft³ | **7.1 %** |
| FV, c = 100 (default) | 0.205 ft | 2,050 ft³ | 3.2 % |
| FV, c = 150 | 0.091 ft | 911 ft³ | 1.4 % |
| FV, c = 250 | 0.033 ft | 328 ft³ | 0.5 % |
| DW SLOT at crown | 1.86 ft (20.7 % W_max) | — (width decays immediately) | — |
| DW SLOT deep (>1.78·y_full) | 0.09 ft (1 % floor) | 900 ft³ | 1.4 % |
| DW EXTRAN | 0 | 0 | 0 |

Two things follow. First, **at c = 50 the celerity knob is inert on this network**: the
requested width exceeds the 5 % cap for every closed conduit ≳4.9 ft in diameter — and the
TwinOaks verification report confirms it:

> `WARNING 108: the Preissmann slot width cap (5% of the section top width) overrides the
> requested FV_SLOT_CELERITY for 2111 of 2111 closed conduits (cap-implied celerity up to
> 67 ft/s); celerities below the cap-implied value have no effect there.`
> — `TwinOakstoFRSFull_thinned_verify.rpt:12`

Second, the FV slot at the cap stores **~5× more water per foot of surcharge head than the
deep Sjöberg slot** (0.45 ft vs 0.09 ft width) — this, not a defect in the solver, is the
mechanism behind "a significant portion of the volume is stored in the slot."

---

## 3. Why FV shows significant slot volume — four compounding causes

1. **The deck's c = 50 maximizes slot width.** `T ∝ 1/c²`, and 50 lands every conduit on
   the 5 % cap (WARNING 108 above). At the default 100 the slot would already be 2.2×
   narrower; at 150, 5×.

2. **Slot water is booked at full weight, and it is the only place surcharge storage can
   go.** FV link volume is `Σ cell_a·dx` with the slot-inclusive conserved area and no
   clamp (`Routing.cpp:1191-1216`); it feeds `Final Stored Volume` and `SYS_STORAGE`
   directly (`SWMMEngine.cpp:7103-7115, 4200-4207, 4667-4677`). Meanwhile flooded
   junctions cap at `full_volume` (`DynamicWave.cpp:3583-3600`) and FV junctions report
   **zero** storage — they are algebraic interfaces whose water "stands in the incident
   cells, already counted through link volumes" (`SWMMEngine.cpp:7063-7100`). This booking
   is deliberate: clamping the volume would delete water the solver has already absorbed
   (the same policy is documented for DW SLOT at `DynamicWave.cpp:155-180`).

3. **FV reports on the Courant-lengthened length** (`NetworkMeshBuilder.cpp:325-344`),
   while DW deliberately reports on the raw input length (`DynamicWave.cpp:2240-2247,
   2379-2384`) — an additional FV-vs-DW volume inflation factor unrelated to the slot.

4. **The signals that would reveal the slot are muted.** `.out` CAPACITY pins at 1.0 above
   the crown (computed from the legacy geometry, `SWMMEngine.cpp:4467`), so it carries no
   slot information. The two signals that do work: **LINK_DEPTH above `y_full` is
   piezometric head** (FV publishes the unclamped `depthOfArea`, `Routing.cpp:1207`), and
   the Link Flow Summary's "Max/Full Depth" (`stat_max_filling`) exceeding 1.0
   (`SWMMEngine.cpp:3795-3800`).

For the specific TwinOaks verification run: most of its 767.7 ac-ft Final Stored Volume is
genuine pipe fill (the network started empty and absorbed ~250 MG of the supply in 12 h),
with conduits surcharged for ~1.9 h near the source. The slot *share* of stored volume is
`Σ T_slot·max(0, h−y_full)·L` — a number the engine does not currently report anywhere;
see the fix list (§7).

---

## 4. Why the Sjöberg taper was not adopted for FV

The recorded and structural reasons, in order of weight:

1. **Celerity is the user contract.** FV's explicit step pays `Δt ≤ Δx/(|v|+c)` directly,
   so the design makes the pressurized celerity the input and derives the width from it
   (Manual §8.4.1: *"Making the celerity the user-facing quantity keeps the accuracy/cost
   trade explicit."*). A Sjöberg width **decays with head**, so `c = √(g·A/T)` **grows
   during a surge** — on the 9-ft main at the 1 % floor, `√(32.2·63.6/0.09) ≈ 151 ft/s`
   and rising with A. The time step would collapse exactly when the network pressurizes.
   DW can afford the taper because its node solve is a point iteration and its momentum
   strips the slot area; it is not CFL-bound on the slot celerity.

2. **The closure stays closed-form above the crown.** The solver carries A and must invert
   `h(A)` and evaluate the pressure integral on every flux call. Constant width keeps both
   exact and linear above `y_full` (Eq. 8-19: `h = y_full + (A−A_crown)/T_slot`;
   `FvKernels.hpp:249`, Manual §8.4.3). A Sjöberg width makes `h(A)` transcendental on an
   unbounded depth domain.

3. **Bounded storage error is an explicit design guard.** The 5 % W_max cap exists *"so it
   can never become the dominant storage and understate a surge"* (Manual §8.4.1). The
   design accepts a bounded, quantifiable storage error in exchange for a bounded,
   user-chosen wave speed.

4. **Static-vs-relaxing is a separate, recorded decision** (Manual §8.4.2): a relaxing
   (DPS-style) slot makes bore speed depend on relaxation history, *"which would destroy
   the Rankine–Hugoniot front speed the method exists to get right."* The crown-crossing
   oscillation gates in `test_fv_solver_network.cpp:337-443` pin this behavior.

---

## 5. Does FV need the conveyance-area strip (DW issue #144) in its momentum computation?

**No — and adopting it would be wrong for a conservative scheme.**

- FV's momentum flux is `Q²/A + g·I₁` with A the conserved variable. The Rankine–Hugoniot
  bore speeds and the Riemann wave-speed estimates require the **same** A in state, flux,
  and speeds. Substituting `min(A, A_full)` inside the flux breaks flux–state consistency
  — the front speeds §8.4.2 exists to protect are corrupted. The slot-FV literature
  (Vasconcelos, León, Kerger, Malekpour) keeps A slot-inclusive and controls error via
  slot width.
- DW may strip conveyance area (`conveyArea`, `DynamicWave.cpp:178-180`, policy docblock
  `:155-180`) because its momentum is a non-conservative finite-difference form: area is a
  coefficient there, not a conserved flux, and there is no shock condition to break.
- The FV error channel is small and width-controlled: internal `u = Q/A` is understated by
  `A/A_full = 1 + T_slot·(h−y_full)/A_full` ≈ **0.7 % per foot of surcharge head** at the
  capped 0.45 ft slot (scales with 1/c²). Friction pairs that u with the frozen `r_full`
  (`ExplicitFvSolver.cpp:1477-1482, 2038-2049`) — a partially compensating bias.
- Reported velocity already uses the legacy `a_full`-clamped area (`SWMMEngine.cpp:4465`),
  so `.out` velocities are not slot-distorted. (Corollary: reported `u·A ≠ Q` above the
  crown — a documentation point, not a bug.)

The correct mitigation is the same as for storage: a narrower slot (higher celerity), not
an area clamp.

---

## 6. How to minimize slot storage — practical levers

1. **Raise `FV_SLOT_CELERITY`.** Storage ∝ 1/c². For TwinOaks, 50 → 150 shrinks the slot
   5× (0.45 → 0.091 ft) and silences WARNING 108. Cost: pressurized-cell Δt ∝ 1/c
   (from the effective 67 to 150 ft/s ≈ 2.2× smaller step in pressurized cells; LTS
   localizes the impact). Note the junction-feedback stability bound is
   **celerity-invariant** (`FvOptions.hpp:244-279`) — lowering celerity was never a
   stability fix, so there is no stability argument for keeping 50.
2. **Read the run's own evidence**: WARNING 108 tells you when the knob is inert; the
   Conduit Surcharge Summary tells you where and how long the slot is actually engaged
   (slot stores nothing below the crown); LINK_DEPTH > y_full is the surcharge head.
3. **Don't chase zero.** The slot *is* the pressurization mechanism — its storage is the
   compressibility surrogate. The physically honest target is "small relative to pipe
   volume at the heads you care about" (≤1–2 % is achievable at c = 150–250), not zero.

---

## 7. Fixes adopted from this analysis

| # | Fix | Status |
|---|---|---|
| B1 | TwinOaks deck `FV_SLOT_CELERITY 50 → 150` (50 was fully cap-inert; WARN 108 on 2111/2111 conduits) | applied and verified — see below |
| B2 | Engine: per-link **slot-storage diagnostic** — accumulate `Σ max(0, cell_a − a_crown)·dx` in `publishFv`, expose as a "Final Slot Storage" informational line under Flow Routing Continuity and a `swmm_link_get_slot_volume` C-API getter | **COMMITTED `dfb52ad6`** — 3 gates (exact closure identity, below-crown zero, DW zero) + 2 falsifiers bitten; 19/19 corpus bit-identical vs verified-distinct base; ctest ×3 green |
| B3 | Opt-in tapered FV slot (`FV_SLOT_SHAPE TAPERED`, Sjöberg width with floor) | **decision-gated** — see §8 |
| B4 | DW manual Ch. 3 says the Sjöberg formula applies to `y/y_full = 1.7`; code and legacy use **1.78** (`Chapter3-DynamicWave.md:634` vs `DynamicWave.cpp:142`, `dwflow.c:661`) | **COMMITTED `dfb52ad6`** (same commit) |

### 7.1 B1 result — the slot was not just misreported, it was distorting the dynamics

Rerunning TwinOaks with only the celerity changed (12-h fill, 502 MGD supply):

| | c = 50 (capped → eff. 67 ft/s) | c = 150 |
|---|---|---|
| WARNING 108 | 2111 / 2111 conduits | **gone** |
| Final Stored Volume | 767.7 ac-ft | 544.4 ac-ft |
| External Outflow | 2.9 ac-ft | **198.8 ac-ft** |
| Flooding Loss | 0.0 | 27.0 ac-ft |
| Peak flow, first main | 501.8 MGD @ 0.78 "capacity" | 493.6 MGD @ 1.14 |

The 223 ac-ft difference in stored volume is not a reporting artifact — the wide capped
slot **absorbed head**. Water that should have pressurized the system and driven delivery
to the downstream end instead sat in slot storage, so the c = 50 run showed almost no
outflow after 12 h and no flooding, while the c = 150 run (5× narrower slot, stiffer and
more physical) delivers 199 ac-ft and reveals 27 ac-ft of node flooding the wide slot was
hiding. The original suspicion — that a significant portion of the FV volume was standing
in the slot — was correct, and it mattered dynamically, not just cosmetically.

**Measured with the B2 diagnostic** (TwinOaks at c = 150, patched CLI): Final Stored
Volume 544.438 ac-ft — identical to the pre-diagnostic run, confirming solution-inertness
on the full model — of which **Final Slot Storage = 106.5 ac-ft (19.6 %)**. Even at the
5×-narrower slot, a fifth of the end-of-run storage in this high-head fill transient is
slot water. Scaling options from here: c = 250 would cut it to ~38 ac-ft (×(150/250)²);
going materially below that is B3 territory.

Two follow-ons for the model itself:
- The revealed flooding is at junctions with ~6–7 ft max depths; if these represent sealed
  manholes on a pressure main, give them `SURCHARGE_DEPTH` (or seal them) so pressurization
  doesn't spill — the flooding may be a model-schematization artifact, now visible.
- The 5 % cap deserves scrutiny in a follow-on: on a network where *every* conduit sits at
  the cap, aggregate slot storage can still absorb a hydraulically significant volume —
  exactly what the cap's rationale says it should prevent. A per-network aggregate check
  (B2's diagnostic makes it measurable) is the right instrument.

---

## 8. The tapered-FV-slot option (design sketch + decision point)

**What it would be:** `FV_SLOT_SHAPE {CONSTANT | TAPERED}`, default CONSTANT. TAPERED uses
the Sjöberg width `0.5423·W_max·exp(−(y/y_full)^2.4)` with the 1 % floor above
`1.78·y_full` (constants shared with `DynamicWave.cpp:125-153`), keeping the existing C¹
crown band.

**What it buys:** deep-surcharge storage falls to the 1 % W_max floor — ~5× below the
current cap, matching DW SLOT's deep behavior; storage near the crown becomes *larger*
than today's capped slot (Sjöberg is 1.86 ft wide at the crown vs 0.45), which is the more
physical shape (elastic storage is largest at the transition).

**What it costs:**
- `h(A)` above the crown loses the closed-form Eq. 8-19 — Brent iteration or an extended
  area table on a bounded head range (needs a max-head assumption or an asymptotic linear
  tail at the floor width);
- the pressurized wave speed becomes head-dependent and **grows** as surcharge deepens
  (≈151 ft/s at the floor on a 9-ft main, more for larger A) — CFL tightens exactly during
  surges, with no offsetting stability gain (the junction-feedback bound is
  celerity-invariant);
- percent-level result shifts on every pressurized deck in the parity corpus
  (re-baselining decision), and the crown-oscillation gates
  (`test_fv_solver_network.cpp:337-443`) must stay green;
- more per-flux work in pressurized cells (table/iteration instead of one division).

**Recommendation:** hold. The TwinOaks observation is fully explained by c = 50 sitting on
the width cap plus the booking semantics, and B1 (c = 150) already brings slot storage to
the same 1.4 %-per-10-ft level the Sjöberg floor would give — without any numerics change.
Commission B3 as its own engine round (with a dedicated handoff plan) only if, with B1
applied and B2's diagnostic visible, the remaining slot share still matters for the
intended use of the results.

---

## Appendix A — side-by-side summary

| | FV | DW `SLOT` | DW `EXTRAN` (default) |
|---|---|---|---|
| Slot exists | always (in the closure) | yes | **no** |
| Engages | area from `0.9853·y_full`, full at `y_full` | width from `0.9853·y_full`, area above `y_full` | n/a |
| Width above crown | `min(g·A_full/c², 0.05·W_max)` — constant | `0.5423·W_max·e^(−(y/yf)^2.4)`, floor `0.01·W_max` past 1.78 | 0 |
| Taper | smoothstep `s²(3−2s)` over the 1.5 % crown band only | the decaying exponential itself | n/a |
| Surcharge mechanism | slot storage | slot storage | node dQ/dH iteration |
| Slot in `links.volume` | yes (`Σ cell_a·dx`) | yes (`0.5(a1+a2)·L`) | n/a (volume ≤ full) |
| Slot in momentum area | yes (conservative flux requires it) | **no** (`conveyArea` clamp, #144) | n/a |
| Slot in hydraulic radius | no (`r_full`) | no (`r_full`) | n/a |
| Reported link depth | **unclamped** (piezometric above crown) | clamped to `y_full` | clamped |
| Knob | `FV_SLOT_CELERITY` (default 100 ft/s) | none | none |
| Diagnostics | WARNING 108 (cap) — slot volume itself unreported (until B2) | none | Conduit Surcharge Summary |
