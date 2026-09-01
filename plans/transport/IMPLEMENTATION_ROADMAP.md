# Unified Transport Program — Implementation Roadmap

**Status:** Living tracking document. Created 2026-08-16; update the Status
column as phases land (validation handoffs are the evidence of record).
**Sequencing authority:** user decisions of 2026-08-15/16 — deliver 1D
transport on existing formulations first, then HydroCouple interfacing,
then 2D, then groundwater; G0 decision-closure early regardless.
**Detail authority per step:** the per-capability plans referenced in each
section. This document orders and tracks; it does not redefine.

Legend: ✅ done/validated · 🔄 in validation · ⬜ pending · ⛔ blocked-on
listed in Deps.

---

## Phase 1 — 1D Transport on existing formulations (ACTIVE)

### 1.1 Eulerian ARD engine (`EULERIAN_ARD_TRANSPORT_PLAN.md` §6)

| # | Step | Status | Deps | Evidence / gates |
|---|---|---|---|---|
| E0 | Promote FV species kernels → `transport/fvkernels/` (verbatim, bitwise) | ✅ `08e7900a` | — | E0 handoff §4; FV benchmark byte-identical |
| — | Pre-existing quality faults (INFLOWS pollutant rows, Cinit, ledger init/final, wet-weather double count) | ✅ `29f1577a` | found during E1 validation | E1 handoff §5.2 |
| E1 | Projection + ArdEngine tracer + `QUALITY_SOLVER` + junction stores + loads split | ✅ `a7824b32` | E0 | E1 handoff §5: gates 3/3, smoke ≤2.5 % continuity |
| E2 | Structure passthrough + loud CFL clamp + CSTR-limit gate | ✅ `04340084` | E1 | E2 handoff §5: 6 gates green; orifice load within 0.3 % of EPA (checked against pre-E2 binary, not vacuous) |
| — | Runtime-API forced quality mass rearchitected into the loader stage (legacy `addExternalInflows` pattern; feeds BOTH engines; E2's ArdEngine special-case + report row reverted; LEGACY forcing delivered for the first time) | ✅ `3f56e47a` | found in E2 validation §5.5/5.7a | gate `ForcedQualityMassFluxRoutesUnderBothEngines` |
| E2b | Storage mixing models (CMSTR+TWO_COMPARTMENT/FIFO/LIFO, LARD-shared token) · FV direct-cell-state (solver accumulates time-integrated face fluxes) · tidal reverse-flow boundary conc | ⬜ | E2; boundary conc with E5 sections | plan §6 E2b |
| E3 | Dispersion activation under all solvers (per-conduit D + FISCHER mode, implicit per-chain; transport.ard component config per D-UT8) | ✅ `7684af53` (validated; deck InitDepth fix — dry elements discard Cinit; gate 11 added: FV_DISPERSION-under-EULERIAN_ARD warning, aliasing deferred to E5 as a semantics call; splitting terminology corrected to LIE (O(dt)); falsifier vii row was empty — reset-on-error overlapped configured-late, now asserted directly; override gate asserts C3 itself) | E2 | 11/11 gates; measured C5 separations: D=100 → 0.0447, override → 0.0372 in C3, FISCHER → 9.27e-4 (liveness), CMS → 0.0300 (the ucf² evidence); C1/C2 bitwise across 721 steps; 14/14 sha256; ASan/UBSan clean |
| E4 | Reaction hook (LIE split per routing step, lesson 13) + kdecay exact-exp — E1 decay warning retired; MSX species TRANSPORTED on the mesh (R6; R4b limitation now LEGACY-only); WALL → LEGACY fallback; MSX inflow conc zero until E5 | ✅ `4df5cc0f` (validated; ONE production defect found+fixed: the np-narrowed load loop swept the store non-negativity clamp along — MSX store masses lost their floor at emptying nodes (−273 oscillation at the outfall-adjacent junction, 0.67 mg/L error in that conduit's MSX rows, pollutants bit-identical); validator's gate 9 (inert-MSX-vs-inert-pollutant symmetric trace, 1.78e-15 post-fix) is the ONLY observer, falsifier ix. Gates 2/6 step-1 premise fixed (sloped chain dilutes ~7% immediately); gate 8 recipe fixed (EUL has NO error control — ok=1 at y=4e7; containment needles disambiguated by element kind) | E2, reactions core | 9 gates + flipped R4 gate; falsifiers i–ix mapped; D-R10 RESOLVED: RK5 kept (1.9× stage cost vs ROS2/BDF2 on 250-cell 6-h chloramine deck, all solvers agreeing — cost is per-step evals at one substep, not stiffness; broader sweep at E5); nh2cl parity re-deferred: needs runnable EPANET-MSX + [TRANSPORT_BOUNDARIES] inlet BC (E5); recorded: evalReactionExpression derefs env.pollutants unguarded (unreachable in production — PUSH_POLLUT only emitted when pollutants exist; watch at R4b/E5) |
| E5a | `[TRANSPORT_BOUNDARIES]` (MSX inlet BCs, VALUE/TIMESERIES on external inflow) + `[TRANSPORT_SOURCES]` (distributed conduit sources, mass/s → conc·ft³/s via kLitersPerFt3) + SCALAR_SCHEME/LIMITER model.ard aliases (model.ard wins; FV_DISPERSION stays warn-only per E3 call; TARGET_DX still open §8) + order-independent post-apply row resolution + bypass-warning extension. **E5 split decision:** config/transport half now; accounting half (E5b) next | ✅ `cbb9d321` (validated; ONE production defect found+fixed: all SIX QualitySolver loaders open `if (np <= 0) return` — a guard about pollutant MASS also blocked external-inflow VOLUME, so MSX-only decks (the nh2cl shape E5a exists for) got ZERO boundary injection; guards relaxed to a shared predicate, mass loops already np-bounded. Gate decks all had pollutants=true — the MOTIVATING configuration wasn't in the gate matrix (lesson 20). E3 suite's deferral-encoding gates retargeted (lesson 21); gate 7 split one-key-per-comparison + gate 10 pins the VALUE-vs-TIMESERIES conversion sites (lesson 22) | E2, IO2, E4/R6 | 10/10 E5a + 11/11 E3 retargeted; 11 falsifiers all mapping; 14/14 bit-identical; ASan/UBSan clean; **nh2cl: engine side VERIFIED by running** (NH2CL 2.0→1.358 monotone, TOC 4.0→3.856, kd=0.15 vs k1=1.3 ordering correct) — the ONLY remaining parity blocker is an external runnable EPANET-MSX reference; per-pathway BC limitation recorded (one node cannot give DWF vs storm different influent conc) |
| E5b + IO3 | Treatment interop at ARD node stores (legacy evaluator on published conc + treated-only absorb) · kdecay reacted-ledger booking · CSV detail sidecar · TARGET_DX resolved (§8; warns under FV routing; same line fixed the E1-era raw-mesh-options SI defect — measured 3.30× on CMS, 66 vs 20 cells) · IO3 save-as carry-alongside · IO5 closed by review | ✅ `721ae60c` (validated; ONE destructive defect found+fixed: the IO3 copy used overwrite_existing and silently REPLACED a different pre-existing config in the destination with warnings=0 — now content-compares and warns on replacing different content, silent on idempotent re-save (lesson 23: my gate wrote into a FRESH directory and could not see a destructive write). Two gates had decks where the feature could not act (lesson 24): treatment removal acts on INFLOW conc — J2 had no external inflow, LEGACY reference confirmed identical outflow 12.119 lb; TARGET_DX 250 on 500-ft conduits = ceil 2 → FV_MIN_CELLS floor 4 = the default. Two claims BOUNDED: treatment's mass_lost is STEP-DEPENDENT (0.413/0.105/0.027 lb at 5/10/20 s — pre-existing legacy defect, both engines, recorded for its own parity round); the ledger NARROWS but does not close on flowing decks (unattributed 12.678 → 6.504 of 22.474 lb; leading suspect: E2 store resync scale-down unbooked) | E5a | 7/7 E5b + 10/10 E5a + 11/11 E3; falsifiers all mapped (viii legitimately empty on CFS, CMS probe = the conversion evidence); 14/14 sha256; ASan/UBSan clean |
| — | **Carried from E5b validation:** (a) treatment mass_lost step-invariance fix (legacy formula divides by dt but books as mass — needs its own parity round, moves every model's reported Mass Reacted); (b) ~~ledger residual attribution~~ **RETIRED by `7b2dfaae`** — the residual was the node-store ordering defect, not an unbooked resync (flowing-deck continuity 25.048% → 0.751%); the remaining ~0.75% is the honest target for any future attribution work; (c) dry-cell source-share accounting (sidecar candidate) | ⬜ (a, c) | E5b | step-invariance gate (vary ROUTING_STEP only, Mass Reacted must hold) |
| E6 | C API (`openswmm_transport.h`) + Python + MCP + INP/GPKG round-trip + parity registries | ⬜ | E5 | G-UT6 |
| E7 | HydroCouple `IModelComponent` wrapper + Composer smoke | ⬜ → Phase 2 | E5, T0-HC | G-UT5 |

### 1.2 Species registry & tuple contract (master plan §4.1/§4.3)

| # | Step | Status | Deps |
|---|---|---|---|
| T0a | Species registry (kinds: POLLUTANT/RESERVED_AGE/RESERVED_TEMPERATURE/MSX_BULK/MSX_WALL); engines re-point (shim retirement) at R6/E4 | ✅ `756afa6e` | — |
| T0b | ~~Source-attribution tuple extension `(mass, vol)` → `(mass, vol, age_vol, enthalpy)`~~ **SUPERSEDED by D-UT10 (user decision, 2026-08-17): parallel per-capability accumulators, not a widened tuple.** The age channel is **DELIVERED** — `water_age_state.node_age_vol_in` (rate, age·ft³/s, the age analogue of `qual_mass_in`) filled by the same seven loader pathways in `7c322a6c`, living beside `qual_mass_in`/`qual_vol_in` rather than inside them. The contract §4.3 exists to make is intact: the seam is the LOADER SET, not the C++ type. **Remaining content is the enthalpy accumulator, which now lands WITH H1** rather than as a separate preparatory step — so T0b no longer stands as an independent row | ✅ age half (`7c322a6c`) / enthalpy half → H1 | T0a | Discrepancy found 2026-08-17 while reading the recommended order: A1–A2 had shipped ahead of their stated prerequisite, which is what exposed that the prerequisite had been satisfied in a different shape |

### 1.3 Shared MSX reaction module (`MULTISPECIES_REACTIONS_MSX_PLAN.md` §5; layouts per D-L1/D-L3 + D-R3 as amended)

| # | Step | Status | Deps |
|---|---|---|---|
| R1 | ReactionSystem registry + `[REACTION_*]` parsers (in `model.rxn`) + duplicate-id refusal + embedded fallback — .msx translator deferred to R3 | ✅ `756afa6e` + `9d0dbbff` (InpWriter names dropped embedded sections — validator's judgment call, broader condition than proposed and rightly so) | T0a, IO1–IO2 | R1 handoff §6: 6/6 gates; 14/14 decks bit-identical MEASURED (the `!empty()` structural argument no longer applied); two gates re-toothed after probes |
| R2 | Compiler + Tier-1 VM (flat token pool, pre-resolved indices) + transactional registry (carried obligation) | ✅ `352638e6` | R1 | R2 handoff §5: 7/7 gates, both probes bit on predicted values; validator found `-2^2` unpinned AND legacy mathexpr broken for it (returns 0 both ways) — pinned pending D-R8 |
| R3 | Integrators + EQUIL Newton + FORMULA + rate units + stiffness ladder + D-R8/D-R9 | ✅ `a3fbc78b` + `7c2c151b` (Jacobian caching) + `eca08593` (RK5 default) | R2 | R3 handoff §5: **as-delivered failed 0/6 on four real numerics defects** (t+=proposed-h; wrong ROS2 weights; BDF2 no error control + invalid fixed-step coeffs under variable h; COUPLING NONE publish bug) — all falsified by the pre-fix run, fixed on instruction; 9/9 post-fix, ASan/UBSan clean, 14/14 decks identical |
| — | **D-R10 (validator, `eca08593`): default SOLVER = RK5.** Measured: at shipping tolerances RK5 beats ROS2 27× on ordinary kinetics and 10× on stiff-at-tight-tol (RK5 is stability-limited ⇒ flat in tolerance); implicit wins only stiff-and-loose. Substep-cap message now names the remedy (ROS2/BDF2 or declare species EQUIL). **E4 obligation: profile against real MSX decks to confirm or kill this default** — the ladder is synthetic | ✅ decided | R3 | R3 handoff §5.7 |
| R4 | qualroute (LEGACY) binding: exact-exponential kdecay, MSX element state (tank/pipe scopes), pollutants readable in expressions, failure containment; G-UT1 parity when unconfigured | ✅ `326b595c` (4 validator fixes) | R3 | R4 handoff §5: changeset correct, gates weren't — no gate could OBSERVE the link-side defenses (topology blindness) and three configs bypassed the component silently (MSX-only blocked by TWO independent guards; ARD/IGNORE_QUALITY now warn). 10/10 post-fix, falsifier sweep table, 14/14 bit-identical, ASan clean. nh2cl-vs-MSX properly deferred to R4b (element-local species make only batch comparable, which R3 already gated) |
| R4b | MSX species TRANSPORT under LEGACY (per-element CSTR advection of msx state) + pollutant kinetics rows | ⬜ | R4 | deferral errors + warnings already in place |
| R5 | **◐ PARTIAL — the C half SHIPPED under labels E-C1/E-C3 and this row was stale** (verified 2026-08-25). `swmm_reaction_validate_expression` exists by exact name at `openswmm_reactions.h:78`, impl `openswmm_reactions_impl.cpp:114`, inside a ~38-entry-point authoring surface (804 LOC impl, `test_reactions_api.cpp` 555 lines): species/coeff/term add-remove-set, `swmm_reaction_expr_set`, init-condition get/set, `check_text`/`apply_text`/`serialize`/`save`, vocabulary discovery. **Missing: the Python and MCP halves only.** Original scope — Python/C/MCP reaction APIs + sympy/decorator authoring + mid-sim editing + `swmm_reaction_validate_expression` (GUI contract) | ⬜ | R3 |
| R6 | ARD cell-path binding (= E4, ✅ `4df5cc0f` — see E4 row) + `[REACTION_SUBCATCHMENTS]` hook (still ⬜, lands with the watershed phases) | ✅ / ⬜ | R3, E2 |

### 1.4 Water age (`WATER_AGE_TRACKING_PLAN.md` §7)

| # | Step | Status | Deps |
|---|---|---|---|
| **⚠ PRIORITY** | **ARD node-store ordering defect — FIXED: "mix before discharge".** Root cause (settled by tracing ONE node's per-substep ledger, answer in a single field `v_preload=0.000000`): the store's donor read happened BEFORE that substep's arrivals mixed in — a forward-Euler CSTR whose stability bound dt·q/V ≤ 2 ordinary junctions exceed at rs ≥ 3 (6.25 at rs = 10). Loss mechanism: faces debited more mass than the store held, the volume clamp zeroed volume while the mass debit stood, the inflow landed on an empty store. Manufacture mechanism: the zero floor clipped the negative half of the instability's oscillation after the receiving cell already took the oversized flux. Fix: loads + E5a boundary mass + face inflows apply BEFORE the donor read; stage 4 keeps only outflow — per-substep totals unchanged (conservative), donor is a weighted average of held+arrived (max principle at ANY step size), zero-volume junctions behave like legacy findNodeQual. Post-fix: 100.000 delivered at rs = 1–120. **Both recorded suspects (E5b's and A1a's — the resync) were WRONG**, one stage downstream of the cause; the resync is untouched. E5b's ledger deck: 25.048% → 0.751% continuity (retires that carry item); sdm_struct_dw_ard WORSENED 2.419% → 4.573% and was explained rather than shipped: the dt→0 limit shows both engines converge to 4.56% — the old mass loss had been cancelling the known unbooked-mass hole | ✅ `7b2dfaae` + `4be378de` (SI conversion as invariance gate) + `fa9babba` (**boundedness in the EXCLUDED regime**: mix-before-discharge is a bounded weighted average only while Qin ≥ Qout, so the draining-storage case was TESTED rather than assumed — max principle holds, peak exactly 100.000 at rs 1/5/20/60 vs pre-fix 296.8/4569/5553/3675; note 296.8 at rs = 1: the defect was BROADER than "above rs 2" — that described one deck shape's symptom, not the bound. The draining deck's ledger does not close in EITHER engine and does not converge under refinement — the signature of terms never written, the E5b-recorded hole shared with LEGACY, not a transport error) | — | 3 new gates in test_ard_node_store.cpp, each verified 0/3 on the pre-fix engine; transport-BC gates fixed at CAUSE (E5a's stage-5b boundary mass had half-applied the ordering invariant — moved beside its volume, not band-widened); SI dispersion floor recalibrated 1% → 0.2% with measured 10.6× discrimination (matches E3's 10.8×), geometric-mean placement, 3.3× margins both ways; 139/140; ASan/UBSan clean |
| — | **Small carry:** the A1a gate decks still pin ROUTING_STEP 1 with a comment describing the now-FIXED defect as live (the fixing changeset owed the unpin, lesson 21). Next validation round: unpin to an ordinary rs, re-verify the analytic bands (the 21600-s shift should now be step-invariant), and retire the stale comment | ⬜ | 7b2dfaae | rs-unpinned water-age gates green |
| A1a | Reserved `__WATER_AGE__` species on the ARD mesh (last row; exact aging cells+=dt / stores+=dt·vol; free mixing/advection/dispersion from the shared kernels) + waterage component (`model.age`, [WATER_AGE_SOURCES] GLOBAL + NODE constant hours) + q·age_source through all SEVEN loader pathways + loadersNeeded += water_age + reactArdStage ns_total guard relaxed | ✅ `7c322a6c` (validated; 3 fixes: value-token bound BEFORE the arity check crashed on a 3-token NODE row (std::length_error under ASan — a user typo aborting the engine); the TIMESERIES deferral was UNREACHABLE in the plan-documented spelling (arity-first ordering rejected it as malformed — deferral checks must precede arity checks when the deferred spelling has different arity); InpWriter now writes WATER_AGE **and** QUALITY_SOLVER (a save-as silently reopened as LEGACY with age off — scope extended by one pre-existing key deliberately). Gate 2 tightened to ±5 s (shift EXACTLY 21600.000 at rs ≤ 2), gate 1 to ±12%; falsifier iv REFUTED §2.3's observer claim (pure-age aging comes from the volume resync, not qual_vol_in); falsifier viii added (store publish had one observer) | E4/R6, E5a | 7/7 gates; 11/11 falsifiers; 14/14 sha256; ASan/UBSan clean across 54 tests |
| A1b | LEGACY CSTR age mirror: routeLegacyAge LAST in execute (single-scalar rerun of accumulate/mix/link-update on the SAME qual_vol_in denominator; no evap factor for age; INITIAL_STATE seeds first step; writes only water_age_state ⇒ pollutants bitwise); A1b warning retired + gate flipped in-changeset; ON+IGNORE_QUALITY warning moved engine-level; pure-age LEGACY decks admitted | ✅ `d2f003e6` (validated; mirror faithful, 10/10 on arrival — the round's value was the INSTRUMENT: §2.4's nominated step-invariance probe was proven BLIND by writing the defect into the mirror (the shift is a DIFFERENCE of two runs; the ordering bias is in both and cancels — lesson 33). The separating observable is the absolute-bias SLOPE: 5·dt−0.5 (one splitting step per element crossed) vs 10·dt−0.5 (two) — gate 13 measures it and is the ordering's only observer. Gate 12 asserts the dt→0 extrapolation (exact residence-time theorem — the strongest correctness claim). Falsifier-iv prediction CORRECTED: the legacy evap factor fires whenever v_new < v_old + v_in (every flowing node at steady state — NOT evap-only; the pollutant c_max clamp hides it; for age it would pin at a_max and never mix down) — omission right for a stronger reason than plan §8 gave. Gate 14 added: the relocated ON+IGNORE_QUALITY warning had NO observer in either home. **A1a unpin DONE**: rs = 5, comment names 7b2dfaae, verified across rs 1–60 first | A1a, `7b2dfaae` | 139/140; 14/14 sha256; ASan clean across 56 tests. **For A5:** (i) ARD's absolute bias is 20.939 + 1·dt — a 2.25% dt-INDEPENDENT residual (new, small, recorded); (ii) LEGACY-vs-ARD link ages differ 6.5–15.2% mostly by DEFINITION (mixed-tank outlet vs volume-weighted cell mean) — cross-engine tolerances must compare the OUTFALL NODE (+4.5 vs +21.9 s), the common quantity |
| A2a | Age hotstart persistence: native format V3 (node/link records carry age, −1 sentinel; save promotes under WATER_AGE; apply restores + both engines seed from LOADED state; ARD snapshots before its resize) | ✅ `f704b83d` (validated; design sound, all falsifiers land — but **both delivered gates were VACUOUS**: `swmm_hotstart_apply` requires INITIALIZED and the gates called it from OPENED, so both exited on SWMM_ERR_LIFECYCLE *before* the first age assertion — nothing about persistence was exercised (lesson 36; invisible to a syntax-only sandbox). Fixed ordering open→initialize→apply→start, which is also correct for the feature since `ctx.reset()` runs in open(). Continuity band tightened (0.25×,200000) → ±10% on measured ARD 876.4→837.9 (−4.4%) / LEGACY 964.1→948.5 (−1.6%); **ARD's larger drop is STRUCTURAL** — one age per link in the record vs one per cell on the mesh, so a save collapses the within-link profile: an ARD restart is continuous but not bit-continuous (lesson 37). Pre-V3 fallback gained a LEGACY leg (the mirror reaches INITIAL_STATE by a different route). Falsifiers iv/vii were green until BOTH flag-set sites were removed — apply() sets them in the node AND link loop (lesson 38) | A1b | 16/16 gates; 139/140; 14/14 decks; native hotstart 229 B byte-identical (only timestamp+CRC differ across runs), version field reads 2 ⇒ promotion correctly gated; 7/7 falsifiers |
| — | **⚠ Pre-existing, flagged loudly (NOT A2a's):** UBSan misaligned `uint32_t` load at `HotStartManager.cpp:246` (the CRC check) — blamed to `4e29c8869` (2026-03-26); A2a is merely the first suite to read a native hotstart under UBSan, so a future sanitizer run will surface it and could be misread as A2a's. One-line memcpy fix. **Also carried:** pollutant hotstart restore absent in both formats (native carries none; legacy .hsf deliberately discards `qual[]`) — a parity DECISION, not a bug | ⬜ | — | memcpy the CRC read; parity decision on pollutant restore |
| **⚠ PRIORITY (blocks A2b)** | **`SimulationSnapshot`'s quality vectors have NO WRITER anywhere in the codebase.** `node_quality` / `link_quality` / `subcatch_quality` are declared in `include/openswmm/plugin_sdk/SimulationSnapshot.hpp`, READ by `DefaultOutputPlugin` (.out) and `GeoPackageOutputPlugin`, and populated by nothing — `SWMMEngine`'s snapshot builder fills depth/head/volume/flows and attaches `pollut_names`, but never the concentrations. Both readers guard with `qi < size()` and fall back to **0.0**, so **every pollutant column in the binary `.out` is written as zero** while the header advertises `n_polluts_` columns and their unit codes. Verified by exhaustive grep: the only other matches are the unrelated `forcing.*_quality_*` arrays. Scope: all pollutant reporting under BOTH engines (the ARD/LEGACY state itself is correct — `links.conc`/`nodes.conc` carry the right values; only the reporting handoff drops them). Found while scoping A2b — this is the lesson-26 shape a third time: adding an age column here would have faithfully reported a broken carrier | ✅ `957a1d62` (validated; 2/2 gates, 347/347 build, prior suites 142/143 — the one failure is the known pre-existing FvEngine mesh-refinement test, proven not collateral since its deck has no [POLLUTANTS] and the fill is np-gated. Confinement measured through the PUBLIC reader after a byte-offset decode misaligned and was discarded: **0 differing non-pollutant cells, 70 differing pollutant cells, 0.0 → 42.0**; no-pollutant deck bit-identical; .rpt continuity identical. Falsifiers: 2 bite, 3 green ALL EXPLAINED — iii (`!IGNORE_QUALITY` guard) is unobservable BY CONSTRUCTION (writer zeroes the column count at DefaultOutputPlugin.cpp:256, so its pollutant loop never runs whatever the snapshot holds) ⇒ genuine defence-in-depth, not dead code; iv green as predicted (no subcatchments); v green for a VALUE reason, not the predicted one — see lesson 39 | — | **Correction banked (lesson 39):** I predicted the interpolation would be inert on a step dividing the report step evenly. WRONG — instrumenting `f_rt` on the gate deck (rs 5 / report 60) shows `f_rt = 0.9` on 18 of 20 reports (`f1_rt = 0.1`; the routing clock sits 500 ms off the report grid) and 1.0 only on the duration-clamped tail. The interpolation is LIVE; falsifier v is green because a level pool has `conc_old == conc == 42`, so any weights return 42. Pinning it needs a transient washoff deck. Also found: `TimestepController::compute_next`'s doc comment still advertises a report-boundary clamp that was already removed — one-line docs fix, carried |
| A2b | Age reports as a trailing `__WATER_AGE__` species column in the `.out`, in HOURS. `reported_species_names`/`n_reported_species()` is the single naming+count truth: the TRANSPORT stride (np) and the REPORTED stride (nr = np + age) are named apart on purpose — conflating them is the family that produced the E4/R6 and A1a defects. Age takes no old/new interpolation (no age-old state exists); unit code 0 with readers keying on the NAME (the `.out` enum has no HOURS slot and widening it breaks every reader); subcatch age stays 0 until A3. WATER_AGE off ⇒ nr == np ⇒ output byte-unchanged | ✅ `d4889329` (validated; age column reads **2.166667 h** against the analytic 2 h + 10 min — the units question settled by the VALUE, not the band. **My "stride razor" was BLIND**: reordering only the NAME list (header says col 0 = age, col 1 = TSS, data still TSS-then-age) left every value assertion passing because the gate read `v[6]`/`v[7]` by fixed INDEX and never looked at a name — and §2.3 had deliberately made the NAME the only way to distinguish hours from a concentration, so the one field consumers must trust was the one field nothing checked (lesson 40). Validator added `read_species_ids()` reading the header bytes; the falsifier now fires. Falsifier v inert and my stated mechanism WRONG — `writeHeader` reads `reported_species_names` from the context and never touches `snap.pollut_names`, so it can only bite GeoPackage | A2a, `957a1d62` | 140/141; 14/14 decks bit-identical; ASan+UBSan clean. Validator-added checks the delivered deck could not make: per-element differentiation on a flow-through chain (ages rise monotonically downstream; the two age engines agree on NODES to <1% — ARD outlet 1.809 h vs LEGACY 1.794 h) and links differ STRUCTURALLY and legitimately (ARD publishes a between-nodes value, LEGACY gives each link its downstream node's age) — now visible in the `.out` for the first time and **owed a manual note** |
| species-ID reader | `swmm_output_get_pollut_id` + `OutputReader` parses the FOURTH ID list the writer always emitted. Closes A2b carry (a) — the name is the sole discriminator for the hours column. Additive; one behaviour change: a malformed species list now fails the open | ✅ `06580dd6` (validated; 5/5 gates, 4/4 falsifiers, no gate strengthening needed. **The risky claim was MEASURED, not reasoned**: all 19 checked-in `.out` files swept with base AND new reader — zero open-failures either way, correct names throughout, *including the legacy 5.3-era `Example1.out` ([TSS, Lead])*, so "the writer always emitted this list" holds across BOTH writers; the four parity baselines have npollut = 0 and exercise the empty-list path. **Then the behaviour change was made concrete** rather than left as "costs nothing today": a corrupted copy (footer intact, first species-name length broken — once past the 1024 cap, once negative) opens happily on the BASE reader reporting `pollut_count = 1` on a garbage block, and is REFUSED by the new one — a silently-misparsed file converted into a failed open. Falsifier iii earned its place: reading the species list one position early returns `"C1"`, a conduit name — positive observational evidence that the species list follows the links, the single assumption that could have made the changeset wrong. Falsifier ii is a genuine heap-buffer-overflow under ASan ⇒ the range check is load-bearing | `d4889329` | 140/141; 14/14 decks; ASan+UBSan clean |
| — | **DECISION NEEDED — Python surface for `pollut_id` is written but UNCOMMITTED.** `_common.pxd`, `_output_reader.pyx` (`pollutant_ids` property + `_read_pollutant_ids`) and the `.pyi` stub all carry it coherently; the validator left it out because §3.3 excluded it from scope and the extension was never built — committing would ship unexercised code. **The hazard is real:** `_common.pxd` is shared with the other agent's rename/gage changeset, so their commit will carry the `extern` while the `.pyx`/`.pyi` may not, leaving a half-bound surface. Worth landing with its own gate (build the extension, assert `pollutant_ids == ["TSS", "__WATER_AGE__"]`) | ✅ CLOSED by `d7ce8efb` | `06580dd6` | Python gate reading the names through the binding |
| — | **Python `pollutant_ids` binding.** `_output_reader.pyx`/`.pyi` + `tests/engine/test_output_species_ids.py` (3 gates) + one typed-surface line. **The predicted hazard MATERIALIZED, benignly and now closed:** `e39185e4` swept my `_common.pxd` declaration in with the multi-column-series work, so HEAD carried an `extern` with no consumer — the half-bound surface I flagged when the C reader landed; this binds it. **Shipped with ZERO mechanical verification** (a `.pyx` has no `-fsyntax-only` equivalent in my sandbox), so supplying that verification WAS the validation work — and the changeset was **accepted unchanged** | ✅ `d7ce8efb` (validated; 3/3 gates, all four falsifiers caught by exactly the predicted gate, zero Cython warnings, prior-suite failure set diff-identical to base: 22 failed/906 passed → 22 failed/909 passed, and the 22 are harness/2D-build artefacts that do not move with the changeset. **Falsifier iii confirmed load-bearing**: reversing the read order leaves `pollutant_count` at 2, so a membership-only assertion would have passed — the ordered equality does real work. **Falsifier iv answered with a venue, not a yes/no** (lesson 47): mypy 2.3.1 fails without the `.pyi` line while the runtime gates still pass 3/3, and CI runs that exact command — but mypy is absent from the conda env, so it is observed in CI and unobserved locally. Two environment traps recorded as lessons 46/48: a silent SkipTest that reads as a pass, and a macOS codesign requirement that extends to the engine dylibs, not just the `.so` files) | `e39185e4` | `PYTHON_POLLUT_ID_BINDING_HANDOFF_2026-08-17.md` §6 |
| — | **⚠ Shared-tree hazard observed (informational):** at 08:22 `python/tests/data/solver/site_drainage_example.out` was 863 B against 56063 B at HEAD (period count 60 → 0) with its `.rpt` gutted by 2231 lines; restored by 08:29 by another re-run. Not caused by any of our changesets (a reader cannot affect what a writer emits), and validation used committed fixtures throughout — but **a run in the shared tree can transiently clobber checked-in fixtures, and anyone staging broadly in that window would commit a dead one.** Argues for the narrow-staging discipline already in use | ⬜ | — | — |
| — | **⚠ Carried from A2b validation (close before users see age):** (a) ~~no modern reader can retrieve species IDs~~ **ADDRESSED above** — `SMO_getElementName(…SMO_pollut…)` exists in the legacy output library, but `swmm_output_*` has no pollutant-ID entry point, `OutputReader` parses subcatch/node/link IDs only, and Python exposes just `pollutant_count`. A consumer sees two columns and a field of HOURS labelled MG/L with no way to tell which is which — this is what makes A2b's name-keyed unit decision unusable today. `swmm_output_get_pollut_id` is small and additive but is NEW API surface, so it was recorded not written; (b) **GeoPackage emits NO species rows at all** — nothing registers pollutant names as variables, so every species row is skipped: the age row will not appear "for free", and neither do the pollutant rows (my handoff §5 predicted the opposite); **Post-species-reader correction: `pollut_id` is NOT what this gap needs** — the gap is on the IN-PROCESS snapshot path, where the names are already correct; nothing registers species in the variables table, so `lookup_variable` returns −1 and every species row is dropped. A reader API plays no part in fixing it. (c) ~~a DRY element keeps aging~~ **FIXED `584d1065`** — but my delivered mask was **INERT ON LINKS, the very elements the defect was reported on**: a dry conduit never reports depth 0 (the DW router floors it at FUDGE = 1e-4 ft) and my threshold was 1e-9, five orders below the floor, so the link branch could never fire and dry conduits still published 6.166667 h. The node branch worked, so half the feature was live and the inert half was the half that mattered. **My liveness assertion caught it** (lesson 24/36 paying off — the gate failed loudly instead of passing vacuously). Corrected predicate: a link is wet if it **HOLDS water OR CONVEYS it** (`volume > ZERO_VOLUME || |flow| > TINY`), read from INTERNAL state because snapshot volumes are user units while ZERO_VOLUME is ft³ — a snapshot-keyed test would be an SI unit bug; both constants are the engine's own, so age is now reported exactly when age was routed. Volume-alone fails for REGULATORS (a pump stores nothing and may report depth 0 while carrying full flow — measured pump 0.124060 h = exactly its upstream node's age); the validator's first fix did that and its own new regulator gate caught it (report-boundary mask keyed on reported DEPTH; state keeps aging so refill + A2a hotstart stay correct; depth not volume because legacy maps junction reported volume to 0 and a volume key would zero every junction age — its own trap gate) — surfaced when a deck delivered no inflow and links reported exactly 6.000000 h of age on water that never existed; needs a wet-mask or a documented convention; (d) pre-existing `HotStartManager.cpp:246` misaligned CRC load reappeared under UBSan; (e) ARD reads TSS 0 at an outfall where LEGACY reads 42 — pre-existing ARD outfall behaviour, NOT A2b (verified: ARD with age OFF already reads 0) | ⬜ | A2b | (a) is the priority — it gates whether age is usable |
| — | **Owed gate (dry-mask validation §6.4) — DELIVERED, 🔄 with checking agent:** the state/report separation was UNOBSERVED — falsifier iv (mask the STATE instead of the report) escapes all 141 tests. Its only real consequence is hotstart fidelity, so the gate is: save a native hotstart from a run whose links are DRY, reload, assert the restored link age is non-zero. Belongs in `test_water_age.cpp` beside A2a's hotstart gates | 🔄 | `584d1065` | `DryElementHotstartCarriesTheAgedState` — bone-dry deck (InitDepth 0, FREE outfall, no inflows) + INITIAL_STATE 6 h; the SAVED link age must exceed 21600 s (state aged while every element was dry) then round-trip bitwise. Test-only. **Its entire value is one falsifier result:** masking the STATE must now FAIL — if it still passes, the gate does not observe what it claims |
| — | **GeoPackage species rows (A2b carry b) — 🔄 DELIVERED, with checking agent.** `update()` always read node/link/subcatch quality and looked each species up per object type; every lookup returned −1 because `populate_default_variables()` is a FIXED hydraulic list and nothing registered model-dependent species names. So a `.gpkg` carried **no quality data at all — pollutants included, not just age** — beside a complete hydraulic record, with no error anywhere. Third instance of the lesson-26 shape after `957a1d62`. `prepare()` now registers the reported species for NODE/LINK/SUBCATCH before the variable-ID cache is built, category QUALITY, real units (the `variables.units` column is free text, so the age row can simply say `hours` — the thing the `.out` enum cannot express). **Decision most worth challenging:** a species colliding with a built-in variable FAILS the open, because `UNIQUE(name, object_type)` means an ignored insert would leave `lookup_variable` resolving to the hydraulic row and write concentrations into e.g. the depth series — corruption is worse than refusal, and the plugin has NO warning channel (`prepare()` takes a const ctx, no logger in the TU), so the choice is binary | ✅ `f37f7dde` (validated, accepted unchanged; 59/59 geopackage gates, 142/143 suite — the one failure is the known FV refinement gate whose fix is uncommitted in the shared tree — 14/14 decks bit-identical, ASan+UBSan clean, 6/6 falsifiers. **My falsifier-vi criterion was WRONG and the validator went past it** (lesson 49): only the return-code/message legs failed and by my own rule that meant "theoretical" — but the fail-loud fix makes `update()` unreachable, so the gate CANNOT see the corruption. Measured on a tolerate build instead: `PROBE depth-series row: category=STATE value=99999.0` — a pollutant concentration sitting in the node depth series, and the surviving hydraulic row is what makes it silent. Number now lives in a comment on the gate that can never assert it. **Lifecycle order verified in the engine, not inferred from the green fixture** (lesson 50): `reported_species_names` is built in `open()` (line 327), `prepare()` runs from `start()` (line 1051) — had the order been reversed, `nr <= 0` returns early and every gate stays green. Falsifier ii vindicated: 101 correct-looking `variables` rows, every species row still dropped. All 26 tracked `.gpkg` fixtures unchanged and none carries a QUALITY row — they are schema/reader fixtures, not products of the output plugin) | A2b | `GEOPACKAGE_SPECIES_HANDOFF_2026-08-17.md`; 4 gates |
| A2c | Age-volume balance row (split out of A2b): the `.rpt` continuity table is MASS-shaped and age is neither a mass nor conserved (it grows 1 s/s everywhere), so the row needs its own definition before implementation | ⬜ | A2b |
| A3 | Subcatchment age (ponded + snow + run-on + iface); GW sub-item rides G1 or defers (no interim structure) | ✅ `b5be8ec3` + **run-on fix `5b2b7418`** (8th gate `RunonFromEveryContributorKeepsAgesAboveTheSource` added there — see the requirement-3 entry in standing findings) (validated; 7 gates — 4 as delivered — 147/148, 14/14 decks, ASan clean. 4 of 12 falsifiers observed as delivered, 8 after the work. **My §4.1 'approximation' was a DEFECT and its deferral premise was FALSE** (lesson 64): `max(0, v_new−v_old)` is exactly ZERO when a subarea sheds as fast as it fills — the ordinary impervious storm — so no rain is ever admitted and the age becomes time-since-first-wetting. On a 100%-impervious zero-depression deck with analytic V/Q = 0.14062 h it read **0.88592 h, 6.3× old**. And closing it needed NOTHING from hydrology: `ctx.subcatches.rainfall` is already published (`Runoff.cpp:295`) and run-on is spread as extra precip (`:331-333`) — the very expression `processSubarea` receives. `v_in = (rainfall + runon/area)·frac·area·dt` gives 0.14022 h, and is what plan §3 asked for literally. **Gate 4 could not fail, so the trap was REMOVED rather than watched** (lesson 66): the `= 0` default is gone and a two-argument `resize` is now a compile error. Falsifier i decisive (S2 1.597 h vs 3 h rain). Grep found no missed resize site — but there are **six**, not the four I counted) | A1; GW part ⛔ G1 | A1; GW part ⛔ G1 | **Three findings that change its shape.** (1) `ctx.subcatches.ponded_depth` is **DEAD** — declared, read twice, written by NO solver; the real water is `RunoffSolver::depth_imperv0/1/perv` (`Runoff.hpp:86-88`), and the area-weighted volume already exists at `SWMMEngine.cpp:3664-3666`. Building on the obvious field would have divided by zero everywhere and looked plausible. (2) **Run-on water carries no age today** — the age path skips `outlet_node < 0` (`QualityRouting.cpp:311-312`) while the FLOW path adds `q_runon` (`SWMMEngine.cpp:1948-1958`). The plan's own A3 criterion is a two-subcatchment cascade, so this gap makes that gate untestable until closed — fifth appearance of the flow-knows/quality-doesn't family. (3) Subarea depths are **not in the hotstart**, so subcatchment age cannot persist even after A3. Structural analogue to mirror is `ponded_qual` (mass, stride np, complete-mix with infil/outflow losses at `SWMMEngine.cpp:2540-2568`), NOT the node-store pattern. **User decisions 2026-08-17:** per-SUBAREA age (3 rows/subcatchment); run-on CARRIES age; hotstart DEFERRED. **Closes a defect, not a gap** — run-on water carried NO age (fifth appearance of flow-knows/quality-doesn't), and `RunonCarriesTheDonorsAge` is the gate that fails on `8b5b3ef5`. A2b's placeholder column retired in-changeset. **Call most worth challenging (§4.1):** the mixing volume is a NET estimate `max(0, v_new−v_old)` because the runoff solver does not publish per-subarea inflow/outflow — it under-mixes when a subarea fills and sheds at once, biasing the age OLD; closing it changes hydrology, not transport. **`resize` gained a defaulted 3rd parameter — a trap**: four two-argument call sites would have silently emptied the new arrays; all swept, gate 4 watches for a fifth. **Owed:** `.out` column gate for the retired placeholder; a magnitude (not directional) cascade gate; plan §8's `WATER_AGE_SNOW` question is untouched AND undeferred — no error names it |
| A4 | LID layer age (generic per-layer species block — heat reuses it) | ✅ `5b2b7418` (validated; 6 LID gates + 1 new A3 gate; 149/150 suite in an ISOLATED worktree — see lesson 71; 14/14 bit-identity; ASan/UBSan over 81 tests in 4 suites clean but for the pre-existing `HotStartManager.cpp:246` misaligned load; 10 of 11 falsifiers observed; zero new warnings) — `A4_VALIDATION_HANDOFF_2026-08-18.md`, `A3_RUNON_FIX_SPLIT_2026-08-18.md` (brief: `A4_IMPLEMENTATION_BRIEF_2026-08-17.md`) | A3 ✅ | 6/6 gates, 10 of 11 falsifiers caught. **MY BRIEF'S §1 PREMISE WAS FALSE** (lesson 69): `f_old_soil/stor/pave` are allocated at `LID.cpp:146-149` and **never read or written**; `f_old_surf` is touched only by the swale's Modified Puls solver where it is the net `dx/dt` — *A3's defective quantity*. Building on them would have reproduced the very defect the brief existed to prevent. The conclusion survived by a different route: complete-mix needs only each layer's INFLOW, and every `batch*Flux` routine already computes it as a LOCAL (`soil_infil`, `soil_perc`, `pavePerc`, `storageInflow`) — now published as `in_surf/in_pave/in_soil/in_stor`. **So A4 does touch hydrology**, which I had assumed it would not; purely additive, all 150 tests including every LID test unchanged. **A4 exposed a defect in the A3 code I committed** (§3.1): `runon_inflow` has THREE contributors (subcatchment cascade, LID underdrain return, outfall return) and A3 filled the age numerator from the cascade alone then divided by the total — on a LID deck that produced arriving water at 3.834 h under a 4 h rain, **younger than anything entering the model**. Sixth appearance of flow-knows/quality-doesn't. Fixed: 4.189/4.197/4.427 h. The A6b drain accumulator was also gated on `np_use > 0`, so a pure-age model never delivered drain water at all | **H5's blocker.** Survey answered the question A3 got wrong: **the LID solver DOES publish its inter-layer fluxes** — `f_old_surf/soil/stor/pave` are stored per-unit fields (`LID.hpp` ~:167-170, written `LID.cpp:824`, read `:792`), alongside `inflow`, `surface_runoff`, `drain_flow`, `evap_loss`, `infil_loss` and the `wb_*` volume balance. **So an exact volume-mixed per-layer age is possible and a net-gain estimate must NOT be shipped** — that was A3's 6.3× defect. Layer state: `surf_depth`, `pave_depth`, `soil_moist`, `stor_depth`. Units are grouped BY TYPE (`LIDManager::group(type)`), so iteration is two-level. Still to survey: absent-layer representation, `storedVolume()`'s scope, whether per-layer quality state exists. **Three decisions owed to the user:** granularity (and whether heat wants the same, since A4 serves H5), drain-outflow age, hotstart |
| A5 | ARD + LARD bindings; engine cross-check gate | ARD leg ✅ (A1-era) · **LARD leg ✅ `9f155227`** (ThreeEnginesAgreeAtTheOutfallNode) | — |
| A6 | Python/C/MCP age surfaces — **C subset (GUI-facing, A6-min) ✅ `d7b6c079`** (X5: source-table CRUD + save; Python/MCP + age STATE getters outstanding) | ◐ | A1 |

### 1.5 Heat transport 1D (`HEAT_TRANSPORT_PLAN.md` §6)

| # | Step | Status | Deps |
|---|---|---|---|
| H1 | `__TEMPERATURE__` species + the D-UT10 enthalpy accumulator through all seven loaders (absorbs T0b's remaining half) + LEGACY CSTR transport of T + trailing `.out` column in °C + `[HEAT_SOURCES]` component. **Transport only — nothing adds or removes energy; §2's flux modules are H2–H4, and ARD warns that its binding is H4** | ✅ `4767aabb` (validated; **THREE defects, all in the first ten minutes — it crashed on its own first gate.** (1) SIGSEGV in `DefaultOutputPlugin::writeHeader`: A2b special-cased the reserved column BY NAME against `"__WATER_AGE__"`, so `__TEMPERATURE__` fell through to `ctx.pollutants.units[p]`, empty on a heat-only deck — fixed by keying on the INDEX, which generalizes (lesson 51). (2) Gates 1–2 then failed their own liveness assertion: `heat_transport` reached `loadersNeeded()` but NOT the routing-step guard in `stepRouting` nor `execute()`'s early return, so a temperature-only deck never ran the quality stage at all — **fourth appearance of that family** after R4/E5a/A1a, both guards commented, both still missed (lesson 52). (3) My gate 3 asserted TSS = 42, A2b's level-pool constant, on a deck that FLOWS and dilutes it to 32.465 — rewritten as a heat-off/heat-on comparison, which is the claim it actually makes (lesson 53). Also: `ProcessComponentsTest` used the heat id as its placeholder for an unimplemented component; moved to LARD. **§5 answered by measurement:** the D-UT10 seam claim holds, and my prediction was wrong instructively — dropping DWF's temperature alone gives **22.5 °C, not 30**, because DWF's VOLUME still arrives; that separability IS the parallel-accumulator claim. ρw·cp confirmed unobservable by two constructions: ×42 on the accumulator alone fails four gates, ×42 on both sides leaves all nine green) | T0a; D-UT10 | `H1_VALIDATION_HANDOFF_2026-08-17.md`; 9/9 gates, 143/144, 14/14 decks, ASan/UBSan clean. **Two places where copying the age mirror would have been wrong, both gated:** temperature is not floored at 0 (sub-zero water is ordinary — the clamp is two-sided) and a dry element's temperature is NOT masked to 0 the way its age is, because 0 °C is a real temperature and could not be told apart from "no water"; the default inlet temperature is 20 °C for the same reason. **Deliberate deviation from the plan text (§3.1):** the accumulator carries °C·ft³/s, not Joules — ρw·cp cancel identically while there are no fluxes, so shipping them would ship two constants no H1 gate could observe being wrong (lessons 39/47). **Falsifier ii is the one that matters**: removing `addTempVolume` from the DWF site alone must fail gate 1 — that is D-UT10's "every pathway contributes at the same seam" under test |
| H2 | SurfaceExchange module (Je latent, Jc Bowen) + RH/wind met plumbing | ✅ `221c5dac` (validated; 7/7, 6/7 as delivered, 144/145 suite, 14/14 decks, ASan clean, 7 of 8 falsifiers caught. **Gate 4 failed and the fix was a SHORTER deck, not a wider band** — my §5(d) advice was wrong (lesson 55). **Falsifier iv is unobservable for TWO independent reasons, one of which I missed** (lesson 56). **Falsifier vi recorded as NOT-RUN rather than passed** — it needs editing shared hydraulics, and falsifier v covers the same gate from the link side; the distinction is lesson 39 applied to falsifiers themselves) | H1 | `H2_VALIDATION_HANDOFF_2026-08-17.md`; 7 gates. **The scoping question is answered and the answer is a precedent, not a new model: heat exchanges exactly where EVAPORATION does** — storage-node free surfaces (`node::getSurfArea`, `Routing.cpp:490`) and open conduits (`getWofY(y)·length·barrels`, `Routing.cpp:597`), nothing else; junctions and closed sections have no free surface by legacy's own convention, which is why they do not evaporate either. Both expressions run under EVERY routing model, so the module is not inert under STEADY/KINWAVE — the lesson-52 shape I was worried about. **ρw·cp become observable here** (gate 4: doubling cp must halve the cooling — the gate H1 could not write). **Second deviation from a stated promise, flagged (§4.1):** H1 said H2 would rescale the accumulator to J/s; I converted the FLUX instead — same observability, one function touched instead of seven loader sites plus H1's gates. Default OFF so H1 decks and 14/14 `.out` files are unchanged. **Owed:** falsifier iv (top width vs `w_max`) is probably UNOBSERVED by this gate set — closing it needs an absolute first-step energy assertion |
| H3 | RadiativeExchange (Jsn, Jan, Jbr, Jlc; RHE formulations, golden files from HydroCouple repo) | ✅ `7038bea9` (validated; 7/7, 6/7 as delivered, 145/146, 14/14 decks, ASan clean, 8/8 falsifiers. **Gate 3 failed and could not have passed either way** — it asserted an invariant conditional on 'equal emissivities' but passed Brunt's Aa COEFFICIENT where an emissivity was meant, making ε_atm = 1.037 (above unity) against ε_lc = 0.97; the sum drifted 6.1055 W/m² per 0.25 of `fsky` **whether the code was right or wrong**, so it failed with AND without falsifier i and discriminated nothing (lesson 57). Fixed by deriving ε_lc from `atmosphericEmissivity()` with the same coefficient plus `ASSERT_LT(eps, 1.0)`. **After the fix falsifier i fails gate 3 and gate 3 alone — the sky-view correction is now a TESTED claim, and the §2.1 physics is confirmed** (invariant holds exactly at 353.525753839 for every `fsky`). Falsifier iv is off by **46,157×** (0.0088 vs 406.18) — absurd as hoped. §2.2 confirmed independently: kPa gives 0.502119 vs 0.567009, both plausible) | H1 | `H3_VALIDATION_HANDOFF_2026-08-17.md`; 7 gates. **Reading `RHEComponent/src/element.cpp` directly rather than the plan summary caught TWO omissions in the plan text, either of which would have shipped as silent physics:** (1) the sky-view factor **splits** the longwave budget — `Jan` carries `fsky`, `Jlc` carries `(1−fsky)`, complementary shares of one hemisphere; the plan's spelling omits it from `Jan`, so an open-sky element double-counts. Gate 3 asserts the physical invariant (total incoming longwave independent of `fsky` at equal emissivities) rather than either term alone. (2) **Brunt's square root takes PASCALS** — `0.0027·√(e_a·1000)`; fed kPa the emissivity is understated by √1000 and still reads as a plausible 0.502 against a correct 0.567, so only a reference value catches it. **Deliberate non-carry:** RHE's depth-attenuated sediment shortwave split is NOT implemented — no sediment column exists until H4 — so H3 keeps all absorbed shortwave in the water (an overestimate in shallow clear water) and refuses `EXTINCTION` by name so nobody configures a split that does not exist. **Owed:** no golden-file parity run against RHE OUTPUTS — gate 1 carries values from the reference's FORMULAS, which catches transcription but not a misread of its inputs |
| H4 | ARD binding — full CSH eq. 4.1; CSHComponent validation case (G-UT3) | ✅ `8b5b3ef5` (validated; 6/6 gates — 4 as delivered — 146/147, 14/14 decks, ASan/UBSan clean over 44 tests. **SIX of eleven falsifiers escaped as delivered, and the reasons outrank the pass count** (lessons 59–63). Gate 1 was UNREACHABLE: it guarded the MSX else-arm and the `nm` subtraction on a deck with **no MSX species**, so the arm never ran and `nm` was never read. **My §3 mechanism was wrong**: the misdirected write lands in the NEXT conduit's slot and the next iteration overwrites it correctly — the array damage is self-healing; only the last conduit's write escapes, one past the end, and the observable is a **heap-corruption crash**, not corrupt data. **Flux magnitude had no observer at all** — 'cooler by >1e-9' is satisfied by an area ×2, ×½ or barrel-blind, and three of my own falsifiers passed everything. **Falsifier viii was not a defect**: `dt_sub = dt/nsub`, so per-substep application is a consistent refinement; my *predicted mechanism* (full `dt` inside the loop) IS the defect and now has gate 6) | H2–H3, E3 | `H4_VALIDATION_HANDOFF_2026-08-17.md`; 4 gates + H1's ARD gate INVERTED (lesson 21 — the warning it asserted is retired here). `__TEMPERATURE__` is now a mesh row after pollutants/MSX/age, inheriting advection, FCT, node mixing, structure passthrough and dispersion for free (every kernel loops `s < ns`), plus a **per-cell** flux stage — the point of the phase, since the LEGACY mirror can only apply H2/H3 per link on one lumped temperature. **The dangerous part is invisible in temperature:** `publish()` ends in an MSX else-branch and derives `nm` by subtraction, so a missing `s == temp_row_` branch or an uncorrected `nm` corrupts the POLLUTANT arrays — hence gate 1 asserts TSS on a heat deck, differenced against heat-off (lessons 14 + 51). Per-cell area is `widthOfDepth(depthOfArea(cell_a)) × cell_dx`, already barrel-scaled by `FvGeometry` (so barrels are NOT applied twice, unlike the LEGACY path), gated on `is_open` — which also disposes of the Preissmann slot, since a closed conduit never exchanges. Also fixed: `SpeciesRegistry::transported_count()`'s kind whitelist omitted RESERVED_TEMPERATURE and had **no callers**, so the staleness was latent — the reason it was still wrong when H4 arrived. **G-UT3 NOT delivered:** gate 2 is an ARD-vs-LEGACY cross-check, weaker than a CSH reference; a diurnal wave also needs the time-varying forcing H3 deferred. Owed with its dependency named |
| H5a | Watershed temperature — per-subarea state (A3 mirror) + run-on + **surface energy balance on ponded subareas** (D-H5a, pulled forward from H6) + **deck-selectable dry-element policy** (D-H5c) | ✅ `65cae8a8` (validated; **10 gates, 5 of 7 failing on arrival**; 150/151 suite in an isolated worktree; 14/14 decks bit-identical; ASan/UBSan clean over 41 tests; 10 of 11 falsifiers after the work, 5 as delivered; zero warnings) — `H5A_VALIDATION_HANDOFF_2026-08-19.md`. **NOTE ON THE HASH:** the round reported `53b95219`, but the staged index had reverted foreign commit `6dde88b0`, so the commit was rebuilt with `commit-tree` on the true parent and the ref replaced — `53b95219` still exists as an unreferenced object; **`65cae8a8` is the one on the branch** (lesson 79). **Delivered code produced NaN** — the surface balance is forward Euler with no stability limit; a 0.52 ft³ film over 27,226 ft² takes a +862 °C step in 60 s and the sequence runs `5 → 182 → −1.8e4 → −3.9e9 → inf → NaN`, out through `subcatch_runoff_temp` into node temperatures and the report. Bounded by refuse-above-`kMaxStepC = 5.0`, **not** clamped (clamping to a driving temperature either freezes the surface at air temperature or oscillates, and both look like physics). Lessons 74–79 | H1 ✅, H2 ✅, H3 ✅ | **Split from H5** on the A1a/A1b precedent — the phase doubled once D-H5a/b landed. **The H5 line was internally inconsistent and that is why this was raised before coding**: both its verify criteria (runoff equilibration, LID conduction) presupposed mechanisms the scope line omitted and H6 deferred, so neither could have been met. **Survey result that SHRINKS it:** "DWF/GW/RDII source temperatures" is already delivered — all seven `HeatSource` pathways are consumed at the loader seam in `QualityRouting.cpp` (checked the writes, not the declarations — lesson 69). **Survey result that constrains it:** subarea area is `RunoffSoA::area × fraction` and `RunoffSoA::area` is subcatchment area **minus the LID footprint** (`Runoff.cpp:197-199`) — using `ctx.subcatches.area` double-counts what H5b will exchange over again. Met forcing is written at `SWMMEngine.cpp:1379/1424/1431` **inside `stepRunoff` before `runoff_.execute` at `:1648`**, so the runoff-clock binding is reachable; but **shortwave is a static constant** (`HeatComponent.cpp:136`), so no gate may assume a diurnal cycle |
| H5b | LID layer temperature — `LidSpecies::TEMPERATURE = 1` on A4's block + **vertical conduction** (D-H5b) + drain at storage temperature | ✅ `1c78e9dd` (validated; 8 gates; **154/154**; 14/14 decks; 112 tests clean under sanitizers; 8 of 9 falsifiers, 5 as delivered; zero warnings). **A4's age values are bit-unchanged under the widened stride** — every layer age, drain age and runoff age across all six A4 decks differs only in the header `species=1` → `species=2`, which was the most dangerous possible outcome and was checked with values rather than a pass count. Lessons 90–95, and **lesson 91 is the worst gate defect of the program**: the conservation gate never computed the conservation | **What D-H5d/D-H5e changed for this phase:** the surface flux is now reached through `heat::netFluxOut(ctx, T)` and stepped with `heat::relaxT` — an LID layer must use both, and must NOT compose its own sum (lesson 81/D-H5e) nor take an explicit step (D-H5d). LID layer volumes are the smallest in the program, which is the whole reason those two landed first. **Conduction is a SECOND operator on the same state**, so lesson 80 applies directly: it must join `J(T)` before the single relaxation, not be applied as a separate sequential step — otherwise it reproduces the split defect inside one phase | A4's `LidLayerSpeciesState` was built for this: a species stride, `LidSpecies::COUNT_ = 1` today, and `resize(units, species, offsets)` with **no defaulted arguments** deliberately. Raising the count and adding a row needs no second array and no change to `layer_index`. **What must NOT be copied from `routeLidLayerAge`:** the `+dt` aging term, the `std::max(a, 0.0)` floor (sub-zero is ordinary), and the `= 0.0` for an absent/dry layer (D-H5c replaces it). **New physics, no in-engine precedent** — the engine has no thermal conductivity at all; follow HydroCouple `GWComponent/include/gwmodel.h:870,885-886`. **Stacking-order landmine:** `LID.hpp:73-77` orders layers `SURF/SOIL/STOR/PAVE`, which is *not* the physical stack; A4's `LidLayer` (`SURFACE/PAVEMENT/SOIL/STORAGE`) *is*. Conduction couples physically adjacent layers, so it must iterate `LidLayer` |
| H6 | Stretch: HTS sediment exchange, solar position, shade, subcatch energy balance | ⬜ | H4 |
| H7 | LARD binding + API/MCP/docs/parity | ✅ **H7 COMPLETE 2026-08-30**: H7a `f31efd63` (row layout) + H7b (temperature on the segments; RWPT-dispersed at the solute coefficient per the ARD precedent, user decision; bypass warning DELETED; 4 gates + wiring flip; corpus now 21 with heat_lard, 20/21 A/B identical and the mover attributed; ctest 180/181 ×3). Two sequence assumptions corrected: hotstart has NO temperature substrate in ANY engine (gate pins the reseed; format widening owed to step 9) and the empty-slab hold has no reachable observer (recorded, not gated vacuously). `H7B_LARD_HEAT_RECORD_2026-08-30.md`. Owed: Kdecay units parity (1/s vs legacy 1/day) | H4, LARD-4 |

### 1.6 I/O & component config architecture (`TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md` §7 — lands WITH Phase 1, not after)

| # | Step | Status | Deps |
|---|---|---|---|
| IO1 | `[PROCESS_COMPONENTS]` handler + registry resolution (toggle-consistency rules land with each component's toggle) | ✅ `64c831d6` | — | IO12 handoff §5: 4 gates; 20/20 decks byte-identical without the section; lenient-open + non-2D build verified. Two gate claims were toothless as delivered (relative-path never exercised, round-trip never called InpWriter) — proven with probes and fixed in the test file by the validator |
| IO2 | Component-file section reader + path rules + apply-hook delivery (embedded fallback lands with R1) | ✅ `64c831d6` | IO1 | same record |
| IO3 | Writers: InpWriter pointer section + per-component `saveData()` + round-trip gate | **◐ PARTIAL** (verified 2026-08-25) | IO2 | **The roadmap contradicted itself here — the E5b+IO3 row says ✅ `721ae60c`, this row said ⬜. Settled from the CODE: both were half right.** The pointer section and IO3 carry-alongside DO exist (`InpWriter.cpp:2520-2569`). Per-component `saveData()` does **not** — `InpWriter.cpp:2574` says so in its own words. **🛑 CORRECTED 2026-08-26: the warning at `:2580-2586` NEVER FIRES IN PRODUCTION.** It is gated on an optional `warnings` sink and **all three production callers pass nothing** (`openswmm_model_impl.cpp:265`, `:276`, `DefaultInputPlugin.cpp:203`). **Saving a model silently destroys embedded `[REACTION_*]` data, from the GUI included** — see the FINDING below. My "the engine warns the user" was wrong: the *writer* can warn; the *engine* never asks it to. A real serializer exists (`ReactionsWriter.cpp`, 183 LOC) but is wired only to the C API, never to `InpWriter`. **Embedded-section round-trip is genuinely broken; the external-file pointer round-trips.** |
| IO4 | GeoPackage embed/extract (D-IO2) | ⬜ (verified absent) | IO3 |
| IO5 | C/Python/MCP component APIs + reload/staleness | **◐ PARTIAL** (verified 2026-08-25) | IO3 | Also disputed — the E5b row said "closed by review". **C half EXISTS**: `swmm_process_component_count/get/find/register/remove`, `openswmm_process_components.h:46-72`, ~110 LOC impl, shipped under label **E-C3**. **Missing precisely:** reload/staleness (zero `reload`/`stale` matches in that header), Python, MCP. |
| IO6 | GUI Files-tab table + FilterKinds + editor↔file binding | ⬜ → GUI plan | IO5 |

**Phase 1 recommended order (single track):** E2 (🔄) → IO1–IO2 → T0a →
R1–R3 → R4 → E3 → E4/R6 → T0b → A1–A2 → H1–H3 → E5+IO3 → H4 → A3–A4 →
H5 → E6+R5+A6+IO5 → gates sweep.

**Order as actually executed (2026-08-17):** E0 → E1 → E2 → IO1–IO2 → T0a →
R1–R3 → R4 → E3 → E4/R6 → E5a → E5b+IO3 → A1a → A1b → A2a → A2b →
(reporting-carry round: snapshot quality fix, species-ID reader C + Python,
dry-element mask + its hotstart gate, GeoPackage species registration).
Two steps ran ahead of their slot: **A1–A2 before T0b** (which is how D-UT10
was discovered — the prerequisite had been met in a different shape, see the
T0b row) and **E5+IO3 before H1–H3**.

**✅ H1 landed (`4767aabb`).** Next per the plan: **H2**
(SurfaceExchange — latent + sensible fluxes, met forcing for RH and wind),
then **H3** (RadiativeExchange). H2 is also where ρw·cp become load-bearing
and observable, and where H1's temperature-volume accumulator rescales to
J/s (H1 handoff §3.1). Note A3–A4 sit *after* H4 in the documented order,
not next.

## Phase 2 — HydroCouple interfacing (justified by coupling to existing CSH/HTS/RHE/GW components)

> **⚠ VERIFIED 2026-08-25 — HC1/HC2 are not "unstarted", they are REFUSED.**
> `PluginType` has exactly four enumerators and none is `PROCESS_COMPONENT`
> (`IPluginComponentInfo.hpp:75`). `hydrocouple_component_info` exists **only
> in two prose comments**; the factory's sole `dlsym` target is
> `openswmm_plugin_info` (`PluginFactory.cpp:253`). `IModelComponent` occurs
> **once in the whole repo — in `paper/paper_v2.md`.** No `HydroCouple*.h`
> anywhere, no `find_package`; the `~/Projects/HydroCouple` checkout is **not
> referenced by the engine build in any form**.
>
> And there is an **active rejection path**: a `[PROCESS_COMPONENTS]` row
> naming a shared library is detected (`ProcessComponentRegistry.cpp:62-73`)
> and hard-errors with "not available yet (arrives with plan phase HC2)"
> (`:216-223`).
>
> **What exists today is an in-process string-id registry with
> HydroCouple-*flavoured* naming** (`org.hydrocouple.openswmm.*`) — no ABI, no
> dynamic loading, no HydroCouple types. That naming is why this phase reads
> as further along than it is, including in our own documents. Costing Phase 2
> as "four ordinary steps" understates it.


| # | Step | Status | Deps |
|---|---|---|---|
| HC1 | HydroCouple 2.0 header dependency (cheap; may land any time) | ⬜ | — |
| HC2 | `PluginType::PROCESS_COMPONENT` + `hydrocouple_component_info()` discovery in PluginFactory | ⬜ | HC1 |
| HC3 | E7: ArdEngine `IModelComponent` wrapper (adapts to kernels, D-UT7) + exchange items + Composer smoke (G-UT5) | ⬜ | HC2, E5 |
| HC4 | Coupled compositions against existing components (CSHComponent stream heat, GWComponent) — early heat/GW results via Composer | ⬜ | HC3 |

## Phase 3 — 2D surface transport (`TWOD_TRANSPORT_PLAN.md` §7; inside integrated2d, GW slots reserved)

> **⚠ NAMING COLLISION.** The steps below are S1–S7. The **snow track** inside
> Phase 1 also uses S1, S2a, S2b, S3, S4 — two live tracks sharing five labels,
> in the same document. Until the snow track is renamed (`SN1…SN4` is the
> obvious fix), every reference to "S3" or "S4" needs its phase said out loud.
> **⚠ LABEL COLLISION, resolved 2026-08-25 — these steps are now `2D-S1…2D-S7`.**
> The snow track inside Phase 1 also uses S1, S2a, S2b, S3, S4, and the two
> shared five labels in this one document. **The unstarted side was renamed,
> not the finished one:** the snow rounds are complete and their labels are
> baked into sixteen handoff documents that are historical record, so renaming
> them would rewrite the record to fix a forward-looking problem. Every
> reference to a bare `S3`/`S4` in a document dated before 2026-08-25 means
> the **snow** round.


| # | Step | Status | Deps |
|---|---|---|---|
| **2D-S1** | SurfaceTransportState + tracer advection on marcher face fluxes (LTS-consistent) + wet/dry conservation + HDF5 vars | ⬜ | E-suite patterns; IO2 (`model.i2d`) |
| **2D-S2** | Dispersion + FCT + BCs + rainfall/evap species rules | ⬜ | 2D-S1 |
| **2D-S3** | 1D↔2D species/age/enthalpy coupling tuples + coupled mass balance | ⬜ | 2D-S1, T0b |
| **2D-S4** | Cell reactions + age + temperature transport | ⬜ | 2D-S2, R3 |
| **2D-S5** | Per-cell surface heat fluxes (reuse H2–H3 modules) | ⬜ | 2D-S4, H3 |
| **2D-S6** | GPU transport kernels behind extended plugin ABI | ⬜ | 2D-S2 |
| **2D-S7** | GW-zone transport | ⬜ → Phase 4 (rides G2) | G2, 2D-S3 |

## Phase 4 — Groundwater (`TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md` §9; G0 decisions close EARLY)

> **⚠ VERIFIED 2026-08-25 — G1/G2 have NO code, and there are two traps.**
> No `openswmm_gw2d.h`, no σ-column, no closure A/B, no GW LTS scheduler. The
> only named artefacts are reserved enum placeholders (`Infil2D.hpp:85-86`)
> that parse and serialize but which **the C API refuses**
> (`ApiInfil2D.cpp:124-130`).
>
> **Trap 1: Track I (per-cell 2D infiltration) IS delivered and is a DIFFERENT
> track** — but `Infil2D.hpp:21` cites the two-zone GW plan as its parent
> document, so a doc-driven audit scores G1 as landed off that one line.
> **Trap 2: LTS in the 2D tree is the surface marcher's tiering**, unrelated
> to the GW explicit-LTS scheduler.
>
> **G0 sign-off (D-N1–N5) has been owed since 2026-08-15** and gates the phase.


| # | Step | Status | Deps |
|---|---|---|---|
| G0 | Plan delivered 2026-08-15; §10 decisions **SIGNED OFF 2026-08-25** | ✅ **CLOSED** | — | D-N1 approved **on condition a 2D corpus deck lands first**. ⚠ **The re-rating risk → chore is RETRACTED (2026-08-26): it counted four fixed sites and there are eleven.** `ExplicitKokkosSurfaceSolver.hpp:137-138` holds `std::array<int,9> tier_off_`/`ftier_off_` — K+1 offsets, so **raising the ceiling to 9 writes index 9 OUT OF BOUNDS** — and `:149`'s occupancy is indexed unguarded. **§11's original risk rating was correct.** D-N2 **deferred to step 18 by decision** — G1 ships conservative and logs the achieved Δt; D-N3/N4/N5 and the carried draft block approved. Two conditions: the ET/infil **migration guide is a MERGE BLOCKER**, and `M_LAYERS 8` is documented as **unrelated to `LTS_TIERS`**. Track I: `INFIL_STEP` = `WET_STEP`; mesh/subcatchment infiltration overlap **warns** and must name both objects. **⚠ §11's "gate 10 bitwise surface regression" DOES NOT EXIST** — the corpus has 19 decks and none is 2D; `test_2d_lts_equivalence.cpp` is conservation/equivalence, not bitwise. Recommendations and evidence: `G0_SIGNOFF_RECOMMENDATION_2026-08-25.md` |
| G1 | Plan steps 1–8: closures A+B standalone (σ-column ALE gates 4–5 first), ET boundary, PER_SUBCATCH + node Darcy exchange (RDII/exfiltration), legacy arbitration | ⬜ | G0 sign-off |
| — | A3/H5 GW sub-items re-point onto G1's PER_SUBCATCH columns | ⬜ | G1 |
| G2 | Plan steps 9–22: lateral Darcy + LTS tier integration (G-A/G-B gates), Dunne/return flow, BC+VG closures, hotstart, benchmarks (αL crossover), `[2D_INFILTRATION]` (11b), APIs (§8.5 `openswmm_gw2d.h`) | ⬜ | G1; runtime tier count (D-N1) touches surface solver |
| T7/S7 | Per-zone (per-σ-layer) solute/heat/age transport + exchange tuples | ⬜ | G2, S3, R3 |

## Phase 5 — LARD (parallel-eligible any time after R3; `LAGRANGIAN_QUALITY_STRATEGY.md` phases 0–7 under §16 layout amendments)

| # | Step | Status | Deps |
|---|---|---|---|
| L0 | `QUALITY_SOLVER LAGRANGIAN` wiring: enum + parser + InpWriter + open() warning + `stepRouting` dispatch to the no-op `LagrangianSolver` skeleton (subplan X1) | ✅ `24602eb2` | — |
| L1–L2 | SegmentStore slabs (D-L2) + LTD advection + junction/storage mixing (subplan X2) | ✅ `8c141a5e` | — |
| L3 | Reaction binding (segments; species-major gather per D-L1) | ⬜ | L2 |
| L4 | RWPT dispersion (counter-based RNG D-L6) — X3a `647a3603` (QUALITY_STEP substepping + dt-reference instrument) · **X3b `b9852cee`** (RwptDispersion.hpp, D-X3b1 penetration-quantum exchange, Elder pinned at factor 3 from seeds {7,8,9,11,13}) | ✅ | L2 |
| L5–L7 | Water age (G6), APIs (`model.lard`, per API strategy as amended), gates G1–G9, perf pass (D-L7) | **◐ PARTIAL** (verified 2026-08-25) | L3–L4 | **Age ✅ `9f155227`** (X4 — reserved species row `np` on the segments, age-before-transport, hotstart-beats-INITIAL_STATE, A5 cross-engine gate). **C API ✅ `d7b6c079`** (X5 — `openswmm_water_age.h`, 8 entry points). **Still open:** L6 perf pass (2000 particles constant, no adaptivity), Python + MCP age surfaces (full A6), gates G1–G9 as a set. |

## GUI track (gated on engine steps; `openswmm.gui/workplans/`)

| # | Step | Status | Deps |
|---|---|---|---|
| **G1g** | Quality/Transport options page | **✅ implemented** (verified 2026-08-25) | `simulationoptionsdialog.cpp:1173` — solver combo, `OUTFALL_BACKFLOW_QUALITY`, LARD group (`QUALITY_STEP`/`MAX_SEGMENTS_PER_LINK`/`DISPERSION`/`RWPT_SEED`), reserved-species checkboxes, "Edit Source Ages…". Documented deviations: ARD group is a **label only**, no 2D group, no heat group |
| **G2g** | Reaction editor | **✅ implemented, substantial** | `reactionsystemeditordialog.cpp`, **1150 lines**, 7 tabs; expression validator `reactionexpressionedit.cpp` (419). Sources tab is a disabled placeholder (engine rejects `[REACTION_SOURCES]`) |
| **G3g** | Water age editor | **✅ implemented AND reachable by two paths** | `wateragesourcesdialog.cpp` (312). **The "Y3b owed / editor unreachable" note is STALE** — Model menu `swmmvis.cpp:3900-3903` and the options-page button both reach it |
| **G4g** | Heat editor | ⬜ **absent** | no heat dialog file exists; only the `HEAT_TRANSPORT` checkbox, an initial-temperature row, and an attribute column. `openswmm_heat.h` is COMPLETE (2026-08-31); the real blocker is component-config serialization (IO3) — API edits to `model.heat` vanish on save |
| **G5g** | Result descriptors (D-G1) | **◐ partial** | `resultdescriptor.h:39-75`, `speciesattributes.h`; consumers across pickers/plots/stats. **Owed:** per-species units from the `.out`; full tabular/statistics round-trip |
| **G6g** | 2D transport rendering | ⬜ **absent** | `ScalarFillSublayer` 0 hits; `swmm2dresultslayer.cpp` renders depth/WSE/velocity only |
| **G7g** | Property edits (storage mixing, dispersivity, BCs/sources) | ⬜ **absent** | 0 hits for any named property |
| GG1–GG7 | INTEGRATED2D_GW_GUI_PLAN (aquifer/infiltration config, water-table/σ-column visualization, LTS diagnostics) | ⬜ | per that plan (GG1 needs G-plan step 1) |

## Phase 1x — the X / Y / Z / closeout tracks (EXPEDITED, 2026-08-23 → 08-25)

**Added to the canonical tables 2026-08-25.** Until then `grep X4|Y0|Z1` in
this file returned **zero hits**: these rounds existed only in
`LARD_AGE_EXPEDITE_SUBPLAN_2026-08-23.md`,
`LARD_CLOSEOUT_PLAN_2026-08-24.md`, Amendment 1 and the individual handoffs.
They are real, validated, committed work and their absence here is the single
largest reason the program's state was unreadable. Every commit below was
**verified present on `swmm6_rel` with a matching subject**, and every
hunk-presence grep the handoffs prescribe reproduces in the tree.

| # | Round | Status | Notes |
|---|---|---|---|
| X1 | LARD wiring (= **L0**) | ✅ `24602eb2` | dispatch live at `SWMMEngine.cpp:3643-3651`. §9.1 self-reports a process failure: the two `SWMMEngine.cpp` hunks that make the option do anything **had never landed** and were written by the validator |
| X2 | LARD segment transport (= **L1–L2**) | ✅ `8c141a5e` | `SegmentStore.hpp` 288 lines. **Deliberate omissions:** evaporation up-concentration and the `c_max` clamp are NOT in the LARD mix (§2.3). Falsifier rows iii, vi, viii returned EMPTY |
| X3a | `QUALITY_STEP` substepping + dt instrument | ✅ `647a3603` | X2.viii left OPEN — falsifier iii cannot bite on the dtq axis by construction |
| X3b | RWPT dispersion (= **L4**) | ✅ `b9852cee` | `RwptDispersion.hpp` 356 lines, splitmix64, D-X3b1 limiter. Elder pinned at factor 3 over five seeds. **No RWPT corpus deck exists** |
| X4 | Water age on segments (= **A5** LARD leg) | ✅ `9f155227` | gate A3's bitwise claim **re-scoped to 1e-9 relative** (mergeability is age-dependent, so partitioning changes) |
| X5 | Water-age C API (= **A6-min**) | ✅ `d7b6c079` | `openswmm_water_age.h`, 8 entry points. **The only round of the subplan whose gates all passed first run.** §10's "THE ENGINE TRACK IS COMPLETE" is **false as written** — Z1 and H1 landed after it |
| X6 | D-NS1 negative sources, all three engines | ✅ `d79c8bcf` | one shared `NegativeSources.hpp` seam. **Found and fixed a pre-existing ARD silent ledger break** (`std::max(0.0,…)` dropping negative loads since E1) |
| Y0 | Transport options C API | ✅ `948b2840` | exists because "G1g is unblocked" had been verified against the **parser**, not the C API — `swmm_options_set(…,"LAGRANGIAN")` returned `BADPARAM`. Recorded a pre-existing process-terminating defect, fixed later by H1 |
| Z1 | Reserved species as `[INFLOWS]` constituents | ✅ `4639be37` | Amendment 1's engine round. **Was started by an unknown session, falsified, and fully reverted within one 15-minute survey before being re-claimed** — see `SHARED_TREE_STATE_2026-08-23.md` §2 |
| H1x | C-API numeric hardening | ✅ `d80bba34` | closes Y0's owed defect |
| Y1, Y2a, Y2b-1/2/3, Y3, Y3b, Y4 | GUI rounds | ✅ | `dae4bad`, `7a5f732`, `9e63357`, `94ff3b5`, `bc4e07c` (gui repo) |
| P0.2 | CHANGELOG | ✅ `2ecdef8a` | `CHANGELOG.md:197-212` |
| P1.1 | X2.viii routing-step instrument | ✅ `6566f407` | the proposed contraction form **fails even clean** (0.899); redesigned to pin the LARD-vs-LEGACY gap at a level |
| P1.2 | Dry-hotstart gate (X4.vii) | ✅ **stale claim — already existed** | `test_water_age.cpp:911`. Carried forward as "owed program-wide" by X3a, X3b, X4, X5 **and** X6 while it was already in the tree |
| P1.3 | C-API numeric audit | ✅ `22e55228` | found a site that **wrote the wrong file slot and returned `SWMM_OK`** |
| P1.4 | `[TRANSPORT_SOURCES]` negative rows | ✅ landed 2026-08-29 (`fix(ard): a negative [TRANSPORT_SOURCES] row is extraction, not a refusal`) | base REFUSED a negative VALUE row at parse (not a silent zero — the handoff's premise was wrong and its own gates could not open their decks); parser now accepts signed source rates, sign carried, per-cell clamp counted+warned (NOT ledgered — MSX species have no ledger row; deferred round). 3 gates, all fail at base; corpus 20/20 + `.rpt` unmoved; 176/177 ×3 isolated (2d_infil writer failure pre-exists at HEAD). **Open:** every extraction deck clamps during fill (40 %-feasible deck: 108 clamps) so the runtime warning fired on ordinary decks — **resolved by P1.4b (2026-08-29): per-clamp warning retired at all three seams, summary enriched with first element + fill caveat, two gate rows flipped; corpus .out AND .rpt unmoved; 176/177 ×3 isolated** (`P1_4B_CLAMP_WARNING_CONTRACT_HANDOFF_2026-08-29.md` CHECK RECORD); TIMESERIES-source gate owed; dry-cell remainder still dropped. `P1_4_NEGATIVE_CELL_SOURCES_HANDOFF_2026-08-29.md` CHECK RECORD |
| P1.5 | Negative DWF/GW/RDII concentrations | ⬜ optional | closeout recommends closing as "won't do" |
| P2.1–P2.5 | Heat under LARD (H7) · L3 MSX on segments · treatment interop · storage mixing beyond CMSTR · full A6 Python+MCP | ⬜ | **each gated behind a live warning** at `SWMMEngine.cpp:342/350/356` |
| P3 | Laminar RWPT deck · **RWPT corpus deck** · `swmmvis_core` extraction | ⬜ | |

**Amendment 1 (`AMENDMENT_1_WATER_AGE_AS_SPECIES_2026-08-23.md`, decision
D-Y4)** — water age becomes a first-class species everywhere the UI shows
species and gains a real inflow pathway, **without becoming a `[POLLUTANTS]`
row**. That distinction is load-bearing: `n_pollutants()` gates the legacy
quality path, and A2b's np/nr stride separation exists precisely to prevent
the conflation family that produced the E4/R6 and A1a defects. Temperature is
deliberately left on the old treatment until its own round.

**⚠ Two label collisions live in this family.** Amendment 1's **Y4** ("age in
the inflow editor") collides with the subplan's pre-existing **Y4** ("LARD
enablement"). And see the Phase 3 note on **S1–S7 vs the snow track's SN
rounds**.

## Cross-cutting gates (master plan §6) — run at every phase boundary

G-UT1 LEGACY bit-parity · G-UT2 cross-solver uniformity · G-UT3 analytical
(Taylor/MSX/CSH) · G-UT4 conservation incl. reversal · G-UT5 Composer ·
G-UT6 API parity registries. CHANGELOG.md at each release (CLAUDE.md §5.2).

## Standing findings to carry (from validation records)

- Pre-existing `test_engine_fv_integration` mesh-convergence failure
  (predates E0; bisected) — owner needed, outside transport scope.
- LEGACY washoff parity questions on `site_drainage_model` (E1 handoff
  §5.7, reaffirmed E2 §5.7a: legacy CSTR creates mass where ARD conserves;
  EPA generates no washoff at all there) — two separate solver-parity
  investigations, own change, no API surface.
- ~~E2 forcing design call~~ — resolved better than either option in
  `3f56e47a`: forced mass delivered in the loader stage per the SWMM 5.2.4
  reference, one path for both engines. The flow-side ledger concern from
  E2 §5.6.2 was re-examined and withdrawn (WRONG CALL — already summed via
  `sum_ext`, verified numerically).
- **IO1/IO2 carried obligations** (validator findings, `64c831d6`):
  (a) **duplicate [PROCESS_COMPONENTS] ids are undetected** — two rows run
  `apply()` twice with undefined precedence; semantics call (error vs
  last-wins) scoped to **R1**, the first implemented component;
  (b) **InpWriter emits `config_path` verbatim**, bypassing the Slice IO-4
  path rebase — an absolute `config=` stays absolute on save-as; scoped to
  **IO3**, whose fix must carry the config file alongside (the
  [2D_MESH_FILE] sidecar pattern), not just rebase the reference;
  (c) `src/engine/CMakeLists.txt` globs without `CONFIGURE_DEPENDS` —
  applying changesets with new .cpp files by patch requires an explicit
  reconfigure or the feature silently doesn't compile (affects every
  future handoff; noted in the handoff template guidance below).
- **R1 carried obligations** (validator findings, `756afa6e`):
  (a) **warnings-channel asymmetry** — the apply-hook signature has an
  `errors` sink but no warnings sink, so components push to `ctx.warnings`
  directly and `push_report_warning`/`emit_warning` never fire: the .rpt
  sees the style warning but API/GUI warning subscribers never do. SDK
  signature decision → scoped to **IO5** (add a warnings sink or a ctx
  warning-emit helper components must use).
  (b) **non-transactional registry population** — `parseSpecies` registers
  MSX species as it parses, so a config that later fails leaves registry
  entries behind; harmless on strict open, but a lenient/editor open holds
  a registry reflecting a rejected file → scoped to **R2** (stage species
  locally, commit to the registry only on success).
  (c) ~~InpWriter `config_path` rebase~~ — resolved in the IO12 follow-up
  `14755a32` (Slice IO-4 rebase; see IO12 handoff §5.8). Carrying the
  config FILE alongside on save-as remains IO3.
- **Handoff-authoring lesson (recurred twice — E1 gate 3, IO1 gate 3):**
  a gate must be probed to fail before its claim is credited. Future
  handoffs: state for each gate WHAT probe would falsify it; validators
  run the probe.
  **Refinement (R1 probes, twice in one round): beware defense-in-depth
  aliasing.** When two independent defenses reject the same input (e.g.
  the registry's duplicate-id refusal AND the component's configured-twice
  guard), a gate matching shared phrasing passes no matter which layer
  fired. Gates must assert wording UNIQUE to the defense under test, and
  the probe must remove exactly that defense and observe the failure.
  **Refinements (R3 round):** (1) *ASSERT inside a table-driven loop
  silently truncates the table* — ROS2's collapse hid BDF2's 248 % error
  entirely; use EXPECT + skip in solver/config tables. (2) *Linear gates
  are structurally blind to stale-Jacobian defects* — for linear systems a
  cached J is exactly the fresh one; any Jacobian-reuse optimization needs
  a NONLINEAR gate (validator's `A' = −kA²` pattern). (3) Numerical-kernel
  handoffs from a non-executing sandbox carry elevated defect risk by
  nature — the R3 gates did exactly their job (0/6 → localized all four
  defects from failure signatures alone); keep gates analytic and
  signature-rich so the validator can diagnose, not just detect.
  **Refinements (R4 round — gate COVERAGE GEOMETRY):** (4) *a gate must
  have an observation path to the defense it claims to cover* — every R4
  deck put the node upstream of the link, so no gate could see the
  link-side defenses; falsifying the in-mix decay zeroing left all six
  green. Check data-flow direction per claimed defense; run the falsifier
  sweep as a TABLE (falsifier × failing gates) so uncovered defenses show
  as empty rows. (5) *enumerate silent-bypass configurations*: for any
  feature gated on modes/options, list every configuration where it does
  NOT run and gate each (warning or defined behavior) — R4 had three
  silent no-op configs, one blocked by TWO independent guards where fixing
  either alone was invisible. (6) *ASSERT when later statements depend on
  the operation* (EXPECT-on-open → segfault instead of a reported failure
  — the R3 lesson's other direction). (7) *discrimination floors need
  measured headroom* — a 0.15 % margin is a check in name only; size the
  physics (k·dt) so separation ≥ several × the band. (8) *falsifier sweeps
  on UNCOMMITTED changesets need verified restoration between cases* —
  `git checkout --` would discard the work itself; the validator's cp
  restore silently failed and was caught only by a 16-digit ratio match
  between supposedly different cases.
  **Refinements (E3 round):** (9) *gate decks must be WET where quality
  matters* — initQuality() seeds Cinit only into elements that already
  hold water, so a deck with InitDepth 0 silently discards its initial
  quality; the liveness ASSERT (`norm > 0`) caught it, the handoff's
  predicted cause ([INFLOWS] format) was wrong. Put liveness asserts on
  every signal a metric divides by. (10) *bypass enumeration runs in BOTH
  directions*: R4's direction is "config present, engine routes around
  it"; E3 validation added the inverse — "engine active, config spelled
  the LEGACY way" (FV_DISPERSION under EULERIAN_ARD was perfectly
  silent; gate 11 + warnIfFvDispersionKeyIgnored). When a new config
  surface supersedes an old key, the old key's silence becomes misleading
  the moment the feature ships. (11) *a falsifier with an EMPTY row can
  mean defense redundancy, not a dead defense* — E3's reset-on-error was
  shadowed by configured-being-set-last, so falsifying it changed nothing
  observable; the remedy is asserting the protected state directly
  (partially-parsed rows under lenient open), not deleting the defense.
  (12) *unused declarations in gate code point at missing assertions* —
  the one new build warning (unused kC3i) marked exactly the observation
  hole the validator then closed (override gate never asserted C3
  itself). (13) *name the splitting scheme correctly*: one full advection
  step then one full dispersion step is LIE splitting, O(dt) — "Strang"
  requires half-steps and is O(dt²). E4's reaction hook must either
  implement true Strang or document the order-1 Lie choice; the plan text
  saying "Strang" is corrected alongside.
  **Refinements (E4/R6 round):** (14) *narrowing a loop's bounds can sweep
  an unrelated invariant that lived in the same body* — the np-narrowed
  load loop was a correct stride fix, but the store non-negativity clamp
  rode inside it and MSX rows lost their floor (−273 mass oscillation at
  an emptying node, donated into the adjoining conduit). When narrowing a
  loop, enumerate every side effect in the body and re-home the ones
  whose domain did NOT narrow. (15) *new row classes need SYMMETRIC
  observers* — a gate watching old rows (pollutants) is structurally
  blind to damage confined to new rows (MSX); the validator's gate-9
  pattern: seed an inert new-row and an inert old-row identically and
  assert they trace the same numbers (1.78e-15 post-fix vs 6.7e-1 with
  the defect). (16) *diagnosis: print the CORRECT quantity beside the
  suspect, normalized* — TSS/10 next to X/8 both reading 0.92942 killed
  the "seeding is wrong" hypothesis in one line and pointed at transport;
  then vary one axis at a time (two pollutants as control, two MSX rows,
  more conduits, ASan last). (17) *containment sites need per-site
  needles* — two containFailure call sites sharing a message left
  falsifier vii empty (lesson-11 shape) until the element kind entered
  the needle. (18) *EUL has NO error control* — it returns ok on a
  blow-up to 4e7 in one substep; failure-containment gates must not use
  EUL as the failure generator unless the state goes non-finite. (19)
  *scale profiling workloads above the noise floor* — the first D-R10
  measurement put all four solvers inside the 43–56 ms hydraulic jitter
  band; a real measurement that answers nothing is not evidence. Resolved
  D-R10: RK5 stays default (1.9× stage cost in the non-stiff regime,
  which is per-step-evaluation-bound: Cash–Karp's 6 vs ROS2's 2 plus a
  cached LU); broader sweep at E5.
  **Refinements (E5a round):** (20) *the MOTIVATING configuration belongs
  in the gate matrix* — E5a was justified by the MSX-only nh2cl shape,
  and every gate deck had a [POLLUTANTS] row; the feature was dead on
  exactly the deck it existed for (all six QualitySolver loaders guard
  `if (np <= 0) return`, blocking external-inflow VOLUME assembly along
  with mass — the lesson-14 shape one layer out: dual-purpose code
  guarded by ONE purpose's predicate blocks both purposes). When writing
  gates, ask "which deck is this phase FOR?" and put that deck in.
  (21) *retiring a deferral error must list the prior gates that ASSERT
  it* — E3's suite encoded the E5 deferral messages and failed on E5a's
  arrival; E4 handled the identical situation deliberately (R4 gate 10
  flipped in the changeset), E5a missed it. Grep prior suites for the
  deferral needle and list the flips in the handoff §1. (22) *one
  invariant implemented in N sites needs N observers or a pinning gate*
  — the mass/s→conc·ft³/s conversion lives at VALUE-resolution AND
  TIMESERIES-evaluation; the "units gate" exercised only VALUE, so
  dropping the ts division passed the whole suite (validator's gate 10
  pins the two sites to measured exact agreement). Related: one
  assertion over two config writes tests their OR, not each — split
  one-key-per-comparison (gate 7).
  **Refinements (E5b round):** (23) *destructive-operation gates must
  start from a POPULATED destination* — the IO3 carry-alongside silently
  replaced a DIFFERENT pre-existing config with warnings = 0, and the
  gate could not see it because it wrote into a freshly created empty
  directory. Overwriting is REQUIRED (re-saves must refresh the copy),
  so the fix announces rather than refuses: content-compare, warn on
  replacing different content, stay silent on the idempotent re-save.
  Test the collision case, not just the clean case. (24) *put the
  feature in its ACTIVE region before gating its effect, and use the
  REFERENCE ENGINE to settle deck doubts fastest* — treatment removal
  acts on the node's INFLOW concentration (cin = qual_mass_in/vol_in),
  so a treated node without external inflow removes nothing (LEGACY on
  the same deck: identical outflow, one comparison, case closed); and
  TARGET_DX 250 on 500-ft conduits lands exactly on the FV_MIN_CELLS
  floor the default already produces. Feature-region checks are deck
  liveness checks (lesson 9) for EFFECTS rather than signals. Also:
  treatment.has_treatment compiles in initQuality(), not at open —
  probe after initialize. (25) *book mass, not mass-rate* — legacy
  treatment accumulates mass_lost/dt into a ledger the report prints as
  MASS, so Mass Reacted varies 0.413/0.105/0.027 lb with ROUTING_STEP
  5/10/20 s. A mass-balance term must be step-invariant; the
  step-invariance GATE (vary only ROUTING_STEP) is the general
  instrument. Pre-existing, both engines, carried for its own parity
  round. And the ledger truth-in-labeling rule: E5b's commit message was
  reworded from "closing the gap" to the MEASUREMENT (12.678 → 6.504 of
  22.474 unattributed) — claim what was measured, name the leading
  suspect for the rest (unbooked store-resync scale-down).
  **Refinements (A1a round):** (26) *the IDENTICAL-CARRIER diagnostic* —
  when a new quantity misses its analytic target, put it and a KNOWN
  quantity through the SAME machinery on one deck (a CONCEN pollutant and
  an EXTERNAL_INFLOW age enter through the same loader shape, q·c and
  q·age): both losing the same 29.4% converts "is the new feature
  broken?" into "the CARRIER is broken" in a single run, and the
  reference engine on the same deck (lesson 24) closes the case.
  Combined with the step-invariance instrument (lesson 25), this found
  the PRIORITY ARD routing-step defect — the age feature was faithfully
  tracking a broken carrier. Also: a plateau under horizon-doubling
  refutes "not yet converged" (bit-equal at 1 h and 24 h ⇒ not an
  equilibration issue). (27) *bind value tokens AFTER arity checks* —
  `toks[has_name ? 3 : 2]` before the size test crashed on a 3-token
  NODE row (and: REBUILD the delivered ordering under ASan rather than
  trusting a reading); *deferral checks must precede arity checks
  whenever the deferred spelling has a DIFFERENT arity* — the TIMESERIES
  deferral was unreachable in the exact spelling the plan documents.
  (28) *a NEW option key ships with its writer line in the same
  changeset* — WATER_AGE missing from InpWriter meant save-as silently
  reopened with tracking off (the lesson-23 family, config edition); the
  validator extended scope by the pre-existing QUALITY_SOLVER key
  deliberately because writing WATER_AGE without it produced a WORSE
  deck. And: falsifier sweeps also falsify the HANDOFF's own observer
  claims — falsifier iv refuted §2.3's stated gate-1 coverage (the
  pure-age deck ages via the volume resync, not qual_vol_in): the sweep
  doing its job on the specification itself.
  **Refinements (node-store fix round):** (29) *trace ONE element's
  per-substep ledger before theorizing* — a single node's RESYNC/SUB
  print settled in one run what two sessions of suspects could not; the
  answer was one field (v_preload = 0.000000). (30) *recorded suspects
  are hypotheses and usually die* — both E5b's and A1a's recorded
  suspect (the step() resync) was WRONG, one stage downstream of the
  cause; keep naming suspects (they focus the trace) but never patch on
  a suspect's authority. (31) *a metric that WORSENS can be the fix
  working* — sdm_struct 2.419% → 4.573% was explained, not shipped: the
  dt→0 limit is the arbiter when the reference engine is useless
  (LEGACY: 9.224 lb out of 0.080 in) — both engines converge to 4.56%,
  and the old defect had been CANCELLING a known ledger hole,
  flattering the total. Never ship an unexplained improvement either.
  (32) *ordering invariants apply to EVERY mass channel or none* —
  "mix before discharge" half-applied (E5a boundary mass moved after
  the donor read while its carrier volume moved before) produced an
  8.0128-vs-8.0 overshoot: when adopting an ordering invariant,
  enumerate all channels (loads, boundary mass, face inflows,
  structures) and move them together. Explicit-CSTR stability
  (dt·q/V ≤ 2) is the general reason donor reads must FOLLOW arrival
  mixing in any store-based scheme.
  **Refinements (A1b round — instrument epistemics):** (33) *a gate that
  works by DIFFERENCING two runs is structurally blind to every error
  the two runs share* — which is most systematic errors. The nominated
  step-invariance instrument passed with the ARD-defect ordering written
  INTO the mirror, because the ordering bias is in both runs and cancels
  in the difference. Test instruments by writing the defect they claim
  to catch. The separating observable is the ABSOLUTE-bias slope: n·dt
  per element crossed per splitting stage (5·dt−0.5 correct vs
  10·dt−0.5 pre-mix donor); gate 13 measures it and is the ordering's
  only observer. (34) *assert the dt→0 extrapolation* — the bias law
  extrapolating to the exact residence-time theorem is the strongest
  available correctness claim (gate 12), and the same measurement
  doubles as a residual detector: ARD shows 20.939 + 1·dt, a 2.25%
  dt-INDEPENDENT residual (recorded for A5). (35) *cross-engine
  tolerances must compare the same DEFINED quantity* — LEGACY publishes
  a mixed tank's outlet value, ARD a volume-weighted cell mean; a
  link-age tolerance would measure a definition (6.5–15.2%); the
  outfall NODE is the common quantity (+4.5 vs +21.9 s). And *a
  relocated warning is exactly when an observer goes missing* — the
  moved ON+IGNORE_QUALITY warning had none in either home until the
  validator's gate 14. Prediction corrected en route: legacy's "evap"
  factor fires whenever v_new < v_old + v_in (every flowing node at
  steady state), hidden on the pollutant side by the c_max clamp — for
  age it would pin the mix at a_max; omitting it is right for a
  STRONGER reason than plan §8 gave.
  **Refinements (A2a round):** (36) *assert the SETUP, not just the
  result* — both hotstart gates called `swmm_hotstart_apply` from the
  wrong lifecycle state and exited on SWMM_ERR_LIFECYCLE before the first
  age assertion: two green-looking gates that exercised nothing. A
  syntax-only sandbox cannot see this. Every API call in a gate's setup
  chain needs its own ASSERT_EQ on the return code (the fixture pattern
  already does this for open/initialize/start — extend it to EVERY
  lifecycle call a gate makes). And when a gate's premise depends on
  lifecycle ORDER, state the required order in the handoff so the
  validator can check it against the API contract. (37) *round-trip
  fidelity is bounded by the SERIALIZED resolution* — the hotstart record
  carries one age per link while the ARD mesh carries one per cell, so a
  save/load collapses and re-uniformizes the within-link profile: the
  restart is continuous but NOT bit-continuous, structurally (−4.4% ARD
  vs −1.6% LEGACY). Say which resolution a persistence format captures,
  and never write a bitwise continuity band across a resolution change.
  (38) *a flag set at N sites needs all N removed to falsify it* — the
  lesson-22 shape for state writes rather than invariants: apply() sets
  hotstart_loaded in both the node and link loops, so single-site
  falsifiers came back green. **Housekeeping lessons the validator also
  contributed:** commit only YOUR changeset when the tree holds two (it
  staged five files and left an unrelated multi-column-series feature for
  its own handoff, validating in an isolated worktree so no result was
  confounded); and blame + flag pre-existing sanitizer findings that YOUR
  work is merely the first to surface (the CRC misaligned load), or the
  next round will attribute them to you.
  **Refinements (snapshot-quality round):** (39) *"unobservable" has two
  very different causes — separate them* — of three green falsifiers, one
  (the IGNORE_QUALITY guard) is unobservable BY CONSTRUCTION through the
  public surface (the writer zeroes its own column count, so the loop
  never runs whatever the snapshot holds): that is defence-in-depth worth
  keeping, NOT dead code and NOT a weak gate. The other two are green for
  DECK reasons (no subcatchments; `conc_old == conc` on a level pool so
  any interpolation weights return the same value) and are review-only
  until the deck exists. Record which kind each is. Corollary: *check the
  mechanism instead of asserting inertness* — I claimed the interpolation
  would be inert on a step dividing the report step evenly; instrumenting
  `f_rt` showed 0.9 on 18 of 20 reports (the routing clock sits 500 ms
  off the report grid), so it is live and required. And *a
  reporting-pipeline defect is invisible to every state-array gate* —
  this defect survived ~20 validated phases because every transport gate
  reads `ctx` directly; when a feature has a USER-FACING surface, at
  least one gate must read back through that surface.
  **Refinements (A2b round):** (40) *gate the field the DESIGN made
  load-bearing* — A2b deliberately gave the age column a concentration
  unit code and declared the NAME the only way to distinguish hours from
  mg/L; the gate then read every value by fixed INDEX and asserted no
  name, so reordering the header's ID list left the whole suite green.
  When a decision makes one field the sole carrier of meaning, that field
  needs its own assertion — read the header bytes if no API exposes it.
  Corollary, and the reason this mattered: *a name-keyed format decision
  is only sound if a reader can actually READ the name* — no
  `swmm_output_*` pollutant-ID entry point exists, so the decision was
  unusable in the modern stack until that API lands. Check consumer
  reachability before resting a design on a field. (41) *predictions about
  OTHER consumers need the same skepticism as claims about your own code*
  — I wrote that GeoPackage would emit the age row "for free"; it emits no
  species rows at all (nothing registers pollutant names as variables).
  And stating a falsifier's MECHANISM invites a correction: mine for
  falsifier v was wrong (the writer reads the context vector, never
  `snap.pollut_names`), which is a better outcome than an unexamined
  green. (42) *a uniform-state deck cannot see a broadcast bug* — the
  level pool gives every element the same age, so per-element
  differentiation needed a flow-through chain; the validator's addition
  also produced the first cross-engine age agreement data (<1% on nodes)
  and exposed a legitimate STRUCTURAL difference on links (between-nodes
  value vs downstream-node value) that now owes a manual note.
  **Refinements (dry-mask round):** (43) *calibrate a threshold against the
  value the SYSTEM produces, not against the ideal* — "dry" does not mean
  depth 0: the dynamic-wave router floors a dry conduit at FUDGE = 1e-4 ft,
  so a 1e-9 test sat five orders of magnitude below the floor and the link
  branch could never fire. The mask was inert on exactly the elements the
  defect was reported on, while the node branch worked — half live, and the
  inert half was the half that mattered. Measure the field on the deck
  before choosing its threshold. (44) *"is this element active" needs every
  way an element can BE active* — a link is wet if it HOLDS water **or**
  CONVEYS it: depth fails for conduits (floored), volume fails for nodes
  (junction reported volume is 0 by convention), and volume-alone fails for
  REGULATORS — a pump stores nothing and may report depth 0 while carrying
  full flow, its age exactly its upstream node's (measured 0.124060 h). Each
  single-field test blanks a whole element class. Prefer the ENGINE's own
  constants (`ZERO_VOLUME`, `TINY`) read from INTERNAL state: snapshot
  volumes are user units while ZERO_VOLUME is ft³, so a snapshot-keyed test
  is an SI unit bug, and using the routing threshold makes age reported
  exactly when age was routed. (45) *state a rationale in parts so the
  wrong part can be discarded* — my "refilling pipe would jump" argument
  did NOT survive: the stale state occupies 0.0107 ft³ against 1263 ft³
  arriving, influence ~1e-5. The hotstart half DID hold, so the
  state/report separation stands on one leg — and that leg was UNGATED
  (falsifier iv escaped all 141 tests). Owed gate: save a hotstart
  from a run with dry links, reload, assert the restored link age is
  non-zero — DELIVERED as `DryElementHotstartCarriesTheAgedState`
  (`test_water_age.cpp`), with the checking agent; the lesson closes only
  when falsifier iv is confirmed to fail. Also: a defence that escapes falsification because the state it
  guards is already 0 (the node mask) is defence-in-depth, not tested code
  — keep it, do not count it (lesson 39's distinction, second instance).
- **Python `pollutant_ids` round (`d7ce8efb`, accepted unchanged).** (46)
  *a suite that skips on ImportError converts "not collected" into "green"*
  — with a non-editable install, pytest puts the rootdir first on `sys.path`,
  `openswmm` resolves to the pure-Python source with no compiled `.so`, and
  the module-level `except ImportError: raise SkipTest` reports
  `collected 0 items / 1 skipped`, which reads as a pass inside a large run.
  **All 28 engine test modules share this shape**, so it is a house
  convention, not a changeset defect — but "the Python engine tests are
  green" is meaningless unless the COLLECTION COUNT is confirmed too. The
  validator moved the source package aside so the wheel under test was
  demonstrably the code being measured. (47) *an observer can be real and
  still out of loop* — refines lesson 39, which separated "green" from
  "unobserved"; this adds WHERE. The `.pyi` line IS observed: `mypy --strict`
  fails without it (`"OutputReader" has no attribute "pollutant_ids"`, exit
  1) while the runtime gates still pass 3/3 — and CI runs exactly that
  command (`typing.yml`). But mypy is **not in the openswmm conda env**, so a
  local run never catches it. Observed in CI, unobserved locally: record the
  venue, not just the yes/no. (48) *build-environment traps are findings too*
  — on macOS the freshly built extension was SIGKILLed on import (137, no
  traceback) because the OS refused the ad-hoc-signed engine dylibs reached
  through `@rpath`; `codesign --force --sign -` over the `.so` files ALONE
  did not fix it, the engine's `install/Darwin/lib/*.dylib` needed signing
  too. Worth having written down before the next person reads a SIGKILL as a
  code defect.
- **GeoPackage species round (`f37f7dde`, accepted unchanged).** (49) *a
  defence makes its own consequence unreachable, so a falsifier cannot
  measure it* — my §4.5 asked which legs fail under falsifier vi and supplied
  a decision rule: if only the return-code leg fails, the hydraulic row
  survived and the corruption was theoretical. **The rule was wrong and the
  criterion was mine.** Only the return-code and error-message legs failed —
  but only because `prepare()` fails, so `update()` never runs and the
  consequence is unreachable *from that test*. Measured directly instead
  (tolerate build, sole species named `depth`, snapshot 99999.0, read back
  through the consumer join): `PROBE depth-series row: category=STATE
  value=99999.0` — a pollutant concentration sitting in the node depth
  series. **The surviving hydraulic row is not evidence of safety; it is
  exactly what makes the corruption silent.** Never infer "the consequence
  does not happen" from "the gate cannot see it" — when the fix prevents the
  consequence, the consequence needs its own probe on a tolerate build, and
  the number belongs in a comment on the gate that can never assert it.
  (50) *a hand-built fixture cannot observe WHEN the engine fills the state
  it assigns* — the gates construct a `SimulationContext` and set
  `reported_species_names` directly. Had the real engine populated that
  vector AFTER `prepare()` ran, `nr <= 0` would return early, nothing would
  register, and **every gate would still be green**. Verified in the engine
  rather than inferred from a passing fixture: built in `SWMMEngine::open()`
  (line 327), plugin `prepare()` called from `start()` (line 1051). Sibling
  of lesson 36 — assert the setup — but about lifecycle ORDER, which a
  fixture erases by construction. Also: falsifier ii earned its billing —
  registering after the variable-ID cache leaves **101 correct-looking
  `variables` rows** and still drops every species row; only a gate reading
  back through `result_timeseries` catches it.
- **H1 round (`4767aabb`, THREE defects — it crashed on its own first gate).**
  (51) *a name-based special case becomes a landmine the moment its category
  gets a second member* — A2b's `writeHeader` tested the species name
  against `"__WATER_AGE__"`; `__TEMPERATURE__` failed that test, fell
  through to `ctx.pollutants.units[p]`, and on a heat-only deck that vector
  is EMPTY: SIGSEGV on gate 1. Fixed by keying on the INDEX
  (`p >= n_pollutants()`), which is right for age, temperature and whatever
  is next. **The procedure this implies:** when you add the SECOND member of
  a category, grep for every place the first was special-cased BY NAME —
  that is the audit I skipped.
  (52) *updating the "does this feature need loading" predicate is not the
  same as updating the guards that decide whether the STAGE RUNS AT ALL* —
  H1 added `heat_transport` to `loadersNeeded()` but not to the routing-step
  guard in `stepRouting` nor to `execute()`'s early return, so a
  temperature-only deck cleared neither and `routeLegacyHeat` never ran.
  **Fourth appearance of this family**: R4 (MSX-only decks), E5a (external
  inflow volume), A1a (water age, on that very line), now H1. Both guards
  carry comments explaining the pattern and both were still missed, so
  comments are not the fix. **Standing step, adopt in every handoff:**
  `grep -rn "options.water_age" src/engine/` and ask of EACH hit whether the
  surrounding context also needs the new flag.
  (53) *a borrowed constant carries its source deck's assumptions* — gate 3
  asserted TSS = 42 (A2b's level-pool `Cinit`) on a deck that FLOWS, where
  zero-TSS inflows dilute it to 32.465. Not a stride slip; an imported
  number whose deck did not come with it. Fixed by comparing against the
  HEAT-OFF run of the same deck, which is the invariance claim the gate
  actually makes. Import the deck with the constant, or make the assertion
  differential.
  (54) *never quote a gate count read from the working tree* — my handoff
  said the water-age suite was 17; the target has 16, because
  `DryElementHotstartCarriesTheAgedState` has been sitting UNCOMMITTED in
  the shared tree for four rounds (see the owed-gate row). Counts for a
  handoff come from HEAD.
  **Two §5 questions answered by measurement, one correcting me:**
  (a) D-UT10's seam claim HOLDS, and my falsifier-ii prediction was wrong in
  an instructive way — dropping only the DWF temperature contribution gives
  **22.5 °C, not 30**. Thirty would need DWF's VOLUME dropped too; 22.5 is
  DWF water arriving with no temperature attached, which is a sharper
  demonstration of the parallel accumulator than my prediction was, because
  it shows the volume and temperature channels are genuinely separable.
  (b) ρw·cp are unobservable in H1, shown by TWO constructions: a factor of
  42 on the accumulator ALONE is observable (1008 °C, four gates fail — the
  plumbing is gated), while the same factor on BOTH sides (the shape §3.1
  describes) leaves all nine gates green. Shipping cp today would have added
  a number only H2 could ever falsify. **Falsifier v was under-specified by
  me**: a true unweighted mean is not expressible at the mixing stage (the
  per-pathway temperatures are already summed by then); the validator built
  it as a constant per-pathway weight of 4.0 — this deck's mean of 6 and 2 —
  which yields exactly the 18 the gate names but is deck-specific by
  construction.
  Also: `ProcessComponentsTest` used the heat component id as its stand-in
  for a not-yet-implemented component, and H1 implemented it — moved to LARD
  (T5) per that test's own comment. Implementing a planned id breaks
  whatever used it as a placeholder.
- **⚠ ACTION OWED (`4767aabb` validation) — the dry-link hotstart gate has
  never landed.** `DryElementHotstartCarriesTheAgedState` exists ONLY in the
  working tree (`git show HEAD:…test_water_age.cpp` has 16 gates and no such
  name); its handoff §6 is still empty four rounds later. The water-age
  state/report separation therefore remains UNOBSERVED, exactly as the
  dry-mask validation recorded. Either validate and commit it or delete it —
  an uncommitted gate is worse than no gate, because it inflates counts.
- **⚠ ACTION OWED (`4767aabb` validation) — the dry-element temperature
  consequence is LIVE.** §3.2's call not to mask dry temperatures is right,
  but a dry pipe now reports its carried temperature indefinitely, exactly
  as dry pipes reported 6 h of age before `584d1065`. Because 0 °C is a real
  temperature this **cannot be fixed inside the current format** — it is the
  strongest argument yet for a per-column NO-DATA sentinel in the `.out`.
  Scope that before H2 puts real flux-driven temperatures in the column.
- **H2 round (`221c5dac`).** (55) *when a gate misses because the physics is
  nonlinear, find the regime where the law is EXACT — do not widen the band.*
  Gate 4 asserted that doubling cp halves the cooling and read 0.5648 against
  0.5 ± 0.02. My §5(d) said "widen it". Wrong: the 1× pool cools 5.1 °C over
  the hour, which lowers its own flux through `e_s(Tw)` and the Bowen ratio,
  so the doubled-cp run loses proportionally more than half. Measured across
  run length rather than argued — 5 min 0.500229 · 15 min 0.502017 · 30 min
  0.511499 · 1 h 0.564816 · 2 h 0.781205 — clean convergence to 0.5. The
  validator added an `end_time` knob, ran the gate at five minutes and
  TIGHTENED the band to 0.005: at that length the law holds to 2e-4, so a
  ratio of 1.0 now misses by a hundred bands instead of twenty-five.
  Widening would have moved the gate the wrong way. **The convergence table
  lives in the gate**, so the choice of regime is justified by data rather
  than by taste. (56) *a falsifier can be unobservable because the DECK makes
  the two branches numerically identical — a different blindness from weak
  assertions, and one no assertion can fix.* Falsifier iv (`getWofY(y)` vs
  `w_max`) is unobserved for two independent reasons. I predicted the second:
  every leg is directional or a ratio, and an over-large area still cools, so
  "did it cool" passes. I missed the first: the gates use RECT_OPEN, and
  `XSectKernels.hpp:908` returns `xs.w_max` for it, because a rectangle's top
  width does not depend on depth. **The two expressions are the same number
  on this deck** — no assertion of any strength could have separated them.
  Re-run with TRIANGULAR (width genuinely varies) it is still 7/7, which is
  the second reason. Closing it needs both a varying-width section AND an
  absolute assertion; recipe recorded below.
- **H3 round (`7038bea9`).** (57) *a gate that asserts a CONDITIONAL
  invariant must assert its condition, or it discriminates nothing.* Gate 3
  claimed "at equal emissivities the total incoming longwave is independent
  of `fsky`" — a real physical invariant — but established the premise by
  passing `0.97` as the third argument of `atmosphericLongwave`, which is
  Brunt's **Aa coefficient**, not an emissivity (`ε_atm = Aa + 0.0027√(e_a
  in Pa)`). That made ε_atm = 1.037, **above unity**, against ε_lc = 0.97,
  so the sum drifted 6.1055 W/m² per 0.25 of `fsky` regardless of whether
  the code was correct: the gate failed with AND without falsifier i.
  Lesson 36 says assert the setup; this extends it to a premise that is a
  RELATIONSHIP BETWEEN COMPUTED QUANTITIES rather than a deck fact. The fix
  derives ε_lc from `atmosphericEmissivity()` with the same coefficient and
  adds `ASSERT_LT(eps, 1.0)` — an emissivity above one is physically
  impossible, so a range assertion on the premise catches an
  argument-meaning confusion instantly instead of as a drifting sum.
  (58) *when retiring a deferral, grep the WHOLE test tree for the phase
  name, not just your own suite.* H2's `FluxModuleTogglesParseAndDefer`
  asserted that `RADIATIVE_EXCHANGE` refuses — true until H3 implemented it.
  My §5.3 falsifier viii covered H3's own gate and missed the other suite's.
  **Second time in three phases** (H1 broke `ProcessComponentsTest`, which
  used the heat component id as its placeholder). The validator's fix is
  better than deletion: **migrate the leg to the next unretired phase**
  (`SEDIMENT_EXCHANGE`/H4) so a live observer survives for the next
  retirement rather than the coverage being spent.
- **⚠ Correction to the H3 validation record — `RHEComponent` IS in the
  mounted HydroCouple checkout.** Validation reported it absent and treated
  the citations as uncheckable. Verified at
  `/Users/calebbuahin/Documents/Projects/HydroCouple/RHEComponent/src/`:
  `element.cpp:113-117` is `computeBackLWRadiation`, `:125` is the Brunt
  line with `sqrt(vaporPressureAir * 1000)`, `:128` multiplies
  `atmosphericLWRadiation` by `skyViewFactor`, and `rhemodel.cpp:43-47`
  gives albedo 0.0, `atmLWReflection` 0.03, `emissWater` 0.97, σ 5.67e-8,
  `atmEmissCoeff` 0.5 — every default H3 ships. Likely a different clone or
  an uninitialised submodule on the validating side. **Consequence: the §6
  golden-file parity gap may be CLOSABLE** — the component source is
  present, so a parity run needs only a build, not a missing dependency.
- **H4 round (`8b5b3ef5`) — the richest failure harvest so far.** (59) *a
  gate for a BRANCH needs a deck that reaches the branch.* Gate 1 guarded
  `publish()`'s MSX else-arm and the `nm` subtraction — on a deck with **no
  MSX species**. Every `s` was a pollutant or a reserved row, so the arm
  never executed, `nm` was never read, and reverting the fix changed nothing
  observable. Two BULK species make it live, and the corruption mode is by
  **stride width, not value**: `msx_node_conc` sized 12 against a reference
  8. Assert SIZES before values when the hazard is a stride. Lesson 20's
  family, one level down: not "the motivating configuration" but "the
  configuration in which the guarded code runs at all".
  (60) *a flushed steady state cannot see its own initial condition, and two
  flushed nothings are not a comparison.* The hour-long run drove TSS to
  1.07e-21; gate 1 compared two of those with a 1e-12 band. At five minutes
  the same deck holds 8.8 / 34.2 / 40.0. Falsifier v (seed from 0) escaped
  for the identical reason. **But the run length is per-CLAIM, not
  per-suite** — the validator deliberately did NOT move the cross-engine
  gate, because at 5 min the CSTR chain reads 19.84 °C against the mesh's
  8.99 and the flushed endpoint is the right instrument for THAT claim. This
  refines lesson 55: different gates in one file can need different regimes.
  (61) *"it moved in the right direction" is not an observer for MAGNITUDE.*
  Gate 3 asserted "cooler by more than 1e-9", which an area twice, half, or
  barrel-blind satisfies equally — three of my own falsifiers passed every
  gate. Closed by comparing ARD's implied area against LEGACY's on a
  **two-barrel** deck (0.0104 °C gap against 0.3411 of cooling): **at one
  barrel a double-applied barrel count is invisible**, so the deck has to
  break the degeneracy the claim lives in.
  (62) *misdirected writes can be SELF-HEALING, which hides the mechanism.*
  §3 said a missing `temp_row_` branch corrupts the MSX/pollutant arrays.
  Falsifier i is observed — but not for that reason. The write lands in
  `msx_link_conc[link*nm + 2]`, the next conduit's slot, which the next
  iteration overwrites correctly; only the LAST conduit's write escapes, one
  past the end. Probed: MSX array bit-identical, `link_temp` frozen at the
  seed, process dies of heap corruption. **The observable was a crash, not
  bad data** — so an assertion on array contents would have caught nothing.
  (63) *a documented distinction that no deck exercises is not a decision,
  it is a comment.* §4.3 justified applying the flux per ROUTING step rather
  than per substep — but every deck here meshes to 12 cells and runs
  `nsub == 1`, so the two were the same code path. At `ROUTING_STEP 60` it
  subcycles four ways. And falsifier viii was **not a defect at all**:
  `dt_sub = dt/nsub`, so a linear source applied per substep is a consistent
  refinement. My *predicted failure mode* — passing the full `dt` inside the
  loop — is the real defect, and now has a step-independence gate.
  Extending (58): `grep -rn "<phase>" tests/` must catch **names**, not just
  messages. The radiative gate was still called
  `TheH3DeferralIsRetiredAndH4IsNot` — strings corrected to H6, but the NAME
  asserted H4 was unretired in the changeset retiring it. Third phase
  running that this grep has earned its place.
  Confirmed by measurement, not reading: §4.1's barrel claim (the mesh is
  already barrel-scaled, so `barrels` must NOT be reapplied). Kept: `is_open`
  over a crown clamp — *a closed conduit is enclosed, so refusing to exchange
  is the physics, not merely slot avoidance*. Falsifier vi escapes because
  the `heat_state.resize` line is genuinely redundant (`assembleExternalLoads`
  sizes it from the same value every step) — kept as documented
  defence-in-depth, not counted as tested (lesson 39).
- **A3 round (`b5be8ec3`) — the most consequential correction to my own
  judgement so far.** (64) *"I'll flag it as an approximation" can be a way
  of shipping a defect, and a deferral premise must be VERIFIED, not
  asserted.* §4.1 said the net-gain mixing volume "under-counts and biases
  old", and that closing it "means the runoff solver publishing per-subarea
  inflow and outflow — a change to hydrology, not transport". **Both halves
  were wrong.** The rule yields exactly ZERO whenever a subarea sheds as fast
  as it fills, which is the steady state of an impervious surface under
  sustained rain: no rain is admitted at all and the age stops being a
  residence time. Measured 6.3× old against an analytic complete-mix V/Q.
  And the inputs were **already published** — `ctx.subcatches.rainfall`
  plus run-on-as-extra-precip is the exact expression `processSubarea`
  receives. I asserted a dependency I had never checked, and the flag made
  the unchecked assertion feel handled. **Before deferring on "it needs a
  change elsewhere", go and look.**
  (65) *a gate deck's FORCING can confine every gate to the one regime where
  the defect is invisible.* The deck rained 5 minutes of 60 — an INTENSITY
  gage on a 5-minute interval reads one value per interval, and the series
  had entries only at 00:00 and 01:00. The mixing term acts only while water
  ARRIVES, so every gate watched a draining surface, the single regime where
  net-gain is exact. `stat_runoff_vol` 1104 ft³ against 36,300 nominal was
  the tell. **My setup assertion checked `any_runoff > 0` — true, and
  useless.** Refines lesson 59: reaching the branch once is not reaching the
  REGIME the claim lives in; assert the duration/magnitude, not the
  existence.
  (66) *a hazard that cannot be observed should be made UNREPRESENTABLE, not
  watched.* Gate 4 guarded the `resize` wipe — but every call site is
  guarded by a size mismatch, so a wipe lands before any age exists and the
  next `routeSubcatchmentAge` re-sizes anyway; reverting each site (including
  `ArdEngine::init`, the only one running after a runoff step) gave
  bit-identical ages. The fix is to delete the `= 0` default so a
  two-argument call is a **compile error** — free, since all callers already
  pass three. Prefer the type system over an observer when the observer
  cannot fire.
  (67) *a conditional selecting between two physical inputs is a DECISION.*
  `a_in = has_runon ? runon_age : a_rain` silently discarded rain whenever
  any run-on existed — and rain outweighed run-on **366:1** on the gate deck.
  Now flow-weighted. I wrote that ternary without noticing it was a choice at
  all, which is how it escaped both the handoff's design section and the
  falsifier table.
  (68) *in a shared tree, check the DIFFSTAT SHAPE against expectation before
  committing.* HEAD moved mid-round (a foreign session landed `5ad66220`),
  and the staged tree would have silently reverted its `CMakeLists.txt` line
  — caught only because the diffstat read `2 +-` where `1 +` was expected.
  **Also recorded:** the 14/14 bit-identity corpus contains **no deck with
  WATER_AGE on**, so "14/14 unchanged" has been structurally incapable of
  observing any age or heat reporting change this whole program — hence
  gate 7 reading the `.out` column by name. Falsifiers ix/x still escape
  (3.2878 / 3.3458 h against a 3.4008 h bracket); the validator left them
  owed **with the numbers** rather than fitting a 1 % margin, which is the
  right call — catching a donor/receiver swap needs more age contrast, not a
  tighter bound.
- **A4 round — my own brief was the thing that was wrong.** (69) *a survey
  that finds a FIELD has not found a VALUE.* The A4 brief's §1 declared the
  LID inter-layer fluxes "published as stored per-unit fields" and told the
  implementer to rely on them. `f_old_soil`, `f_old_stor` and `f_old_pave`
  are **allocated and never touched**; `f_old_surf` is written only by the
  swale's Modified Puls solver, where it holds the net `dx/dt` — **exactly
  A3's defective quantity**. I had grepped the declarations and the two
  lines that mention `f_old_surf`, seen `assign()` plus a write, and inferred
  a publishing contract from array names. A declaration proves storage
  exists; only a **write on the path you need** proves the value is the one
  you want. This is lesson 64's sibling and arguably worse: 64 was a
  premise I never checked, 69 was a premise I checked badly and then wrote
  into a document instructing someone else.
  (70) *"this phase will not touch subsystem X" is a prediction, not a
  constraint.* The brief asserted A4 need not modify hydrology. It did — the
  per-layer inflows exist only as locals inside `batch*Flux`, so publishing
  them is a hydrology change. Purely additive and all 150 tests unchanged,
  but the scoping claim was wrong and the right response was to make the
  change rather than to contort the design around a boundary I had invented.
- **A4 validation round (`5b2b7418`) — three lessons, and the first one
  invalidates a number I had been reporting.**
  (71) *a suite result from a tree with foreign edits is not attributable in
  EITHER direction — foreign edits can MASK a pre-existing failure, not only
  cause one.* The implementation round reported **150/150** from the main
  tree. The same changeset in an isolated worktree at `d85429fb` gives
  **149/150**: the known bistable FV gate failing **to the digit**
  (`0.055224237275644343` vs `0.052534507871460516`, identical to what the A3
  round measured at its own base). Nothing A4 did — the foreign edits had been
  hiding a failure that was already there. Every prior "N/N" taken from the
  main tree should be read with this in mind; the isolated worktree is not a
  formality, it is the only tree in which a count means anything.
  (72) *an assertion with no reference value cannot rot.* The new A3 gate
  asserts only that arriving run-on is **not younger than the RAINFALL
  source** — an impossibility, not an inaccuracy. It needs no expected number,
  so no deck change, no timestep change and no future phase can stale it. This
  is the shape to prefer over a band wherever a physical floor or ceiling
  exists; contrast H2 gate 4, where finding the exact regime (lesson 55) was
  the only way to get a trustworthy bound.
  (73) *which half of a split fix is the bigger error is a MEASUREMENT, not an
  intuition — and mine was backwards.* `A3_RUNON_FIX_SPLIT §2` reasoned about
  separability correctly and about magnitude not at all. The falsifier sweep
  measured it: zeroing the **outfall** age gives **0.348 h**, zeroing the
  **LID-drain** age gives **3.968 h**, against a 4 h rain. The outfall return
  dominates run-on on that deck, so option (2) — the partial fix I had
  recommended a turn earlier — would have removed the **larger** error, not
  the smaller one. The recommendation to land A4 whole survives on the
  guarding argument alone (the deck that observes the fix needs A4's LID to
  exist), but it survived for a reason I had not established.
- **⚠ REQUIREMENT 3 WAS LOAD-BEARING — recorded because it nearly was not
  written.** The A3 gate `RunonFromEveryContributorKeepsAgesAboveTheSource`
  (one subcatchment, a bioretention underdrain returning as run-on, and
  `[OUTFALLS] OUT 9.0 FREE NO S1` routing discharge back, so **all three
  contributors are live at once**) is the **only** thing in the suite that
  catches falsifier xi. That falsifier failed nothing in the implementation
  round; it was the one carried as *owed*. A fix whose guarding deck is
  optional is a fix that will be un-fixed by the next refactor.
- **⚠ A3 (`b5be8ec3`) shipped with a defect that A4 found — recorded against
  A3, not A4.** `subcatches.runon_inflow` has **three** contributors: the
  subcatchment cascade, LID underdrain return, and outfall return. A3's
  `addRunonAge` filled the age numerator from the cascade ONLY and then
  divided by the full rate — so on any deck with a LID or a returning
  outfall the arriving age was diluted toward zero. Measured: **3.834 h
  under a 4 h rain, younger than anything entering the model** (subarea ages
  3.644 / 3.653 / 3.883 h). A3's gates could not see it because **no A3 deck
  had a LID or an outfall return** — lesson 59's shape again, at the level of
  a whole contributor rather than a branch. Both contributors now carry an
  age (4.189 / 4.197 / 4.427 h after the fix). **Fixed and guarded in
  `5b2b7418`**, as one changeset with A4 — the LID half is not separable
  because the drain's age *is* a per-layer quantity and only exists once A4's
  `lid_layer_state` does. Relative severity, measured not guessed (lesson 73):
  outfall **0.348 h**, LID drain **3.968 h**, against a 4 h rain. Also found:
  the A6b drain accumulator was gated on `np_use > 0`, so a **pure-age model
  never delivered drain water at all** — the lesson-20 configuration trap,
  seventh instance.
- **⚠ BLOCKING CONTEXT for every LID number in this program — issue #131.**
  A conventional `[LID_CONTROLS]` block reaches the solver **unconverted**: a
  soil layer specified in inches arrives as **18 ft** with a **0.5 ft/s**
  conductivity, 43,200× too fast. A4's first draft had all four setup legs
  fire because the column drained every drop within one step. The A4 gate
  decks are therefore written **in feet and ft/s** with a `@warning`
  explaining why; **they will fail loudly when the conversion lands, which is
  correct** — do not "fix" them by loosening bands at that point, convert the
  deck.
- **H5a round (`65cae8a8`) — the changeset did not run, and all four of my
  anticipated failure modes were wrong about why.**
  (74) *a shared-resource path hardcoded in a test HELPER couples every gate
  to the FIRST gate's setup.* `write_deck` emitted
  `config="_h5a.heat"` while gates 2–6 each wrote their own
  `_h5b.heat`…`_h5f.heat`. Five of seven gates therefore ran against gate 1's
  config — fluxes off, RAINFALL 8 °C, no policy key — which is why `8`
  appeared where 12, 20 and 35 were expected. **The sharpest part: gate 6
  asserts that a bad policy value is REFUSED, and it "passed" only after the
  fix, because the file containing the bad value was never read.** A gate
  asserting a refusal is uniquely vulnerable to this, since "never read" and
  "correctly rejected" produce identical observations.
  (75) *a guard that NAMES the hazard can still test the wrong predicate,
  and naming it creates the impression it is handled.* `deltaT`'s own
  docstring says a film of water "has no thermal mass and would otherwise
  take an unbounded excursion in one step" — and its guard is
  `heat_capacity > 0.0`, which catches an EXACTLY-zero volume and not the
  near-zero one the sentence describes. I read that docstring while writing
  H5a and took the hazard as covered.
  (76) *lesson 61 at full strength.* Gate 2's "it moved" assertion was
  satisfied by the sequence `5 → 182 → −1.8e4 → −3.9e9 → … → inf → NaN`. An
  inequality against a starting value cannot tell physics from divergence;
  every passing deck was already taking a **+1388 °C** first-wet-step
  excursion and surviving only because rain deepened the surface fast enough.
  (77) *making a defect unrepresentable in the CODE is not making it
  observable in the GATES — and this is exactly how A3 shipped.* §4.1's
  (Σq·T, Σq) pair design was correct and **completely unguarded**: on every
  delivered deck the cascade is the only run-on contributor, so
  `subcatch_runon_temp_rate` and `subcatches.runon_inflow` hold bit-identical
  values (7.68722753 both). Falsifier iii — swap the divisor, i.e. re-arm
  A3's defect — **could not fail**, contrary to my prediction that it would
  fail gate 4. A guard needs a deck on which the right and the wrong answer
  DIFFER; A3's decks lacked one for the same reason, which is why the defect
  lived a whole phase. New gate 8 splits the contributors so each has a
  witness, and closes falsifiers iv and x as well.
  (78) *an instability found in one binding is a property of the OPERATOR,
  not the binding.* **H2's node and link path calls the same unbounded
  `deltaT`** and is unexposed only because those volumes are large. H5b's LID
  layers are smaller again. Recorded as an action, not an observation.
  (79) *a tell that fires must be a HARD STOP, not a look.*
  `git diff --cached --numstat` read `1  1` where §5.2 said expect `1  0`;
  the check fired, and the round committed past it, reverting foreign commit
  `6dde88b0` entirely. Recovery worked — `commit-tree` on the true parent,
  ref replaced, both foreign commits verified intact and all ten non-CMake
  blobs verified byte-identical to the validated worktree — but the protocol
  had written the check as something to read rather than something to obey.
  **Every future handoff states this as a stop condition.**
  *Scored:* my §5 flag on falsifier **vii** was right (gate 1 did not observe
  the seed). All four §5 anticipated failure modes were wrong, including
  §5(a) — `[TEMPERATURE] TIMESERIES` parses fine at
  `HydrologyHandler.cpp:214`. The real defects were in two layers I did not
  think to anticipate: **the test harness itself, and the numerics of an
  operator I was reusing rather than writing.**
  *Sweep:* 10 of 11 after the work, up from 5 as delivered. Only falsifier
  vi still escapes; it needs a transient reference, as predicted.
- **~~⬜ ACTION OWED (H2) — the unbounded explicit step~~ — RESOLVED by
  D-H5d (`5cc83f94`).** Semi-implicit relaxation at all four bindings.
- **D-H5d round (`5cc83f94`) — the scheme is sound; the SPLIT is not.**
  (80) *a property proved per-operator does not survive composition.*
  `applySurfaceExchange` and `applyRadiativeExchange` are separate entry
  points, each now relaxing **fully toward its own equilibrium**. Under
  forward Euler the two increments were linear and added exactly; under
  relaxation they do not commute, so with both modules on the pair overshoots
  the true combined equilibrium **and the answer depends on module order**.
  Measured — two equal modules with equilibria at 30 °C and 10 °C, true
  combined 20 °C, from 5 °C:

  | k·dt | split (node/link) | combined (subarea) |
  |---|---|---|
  | 4.1e-3 | 5.061317 | 5.061359 |
  | 0.41 | 9.700850 | 10.044256 |
  | 39.4 | **10.000000** (the LAST module's equilibrium) | 20.000000 |

  Not a regression — that regime diverged outright before — and it needs
  `k·dt ≳ 0.4`. **Swapping an integrator under an existing operator split
  silently changes what the split MEANS**; the old scheme's linearity was
  load-bearing and nobody had written that down.
  (81) *naming a hazard for a hypothetical future module is not checking
  whether it already holds between two existing ones.* My own handoff §6
  warned that H6's `SEDIMENT_EXCHANGE` "must join `netFluxOut`, not get its
  own inlined conversion" — while SurfaceExchange and RadiativeExchange were
  **already** separate relaxations, in the same file, in the changeset I was
  writing that warning about. I generalized the hazard forward and never
  looked sideways.
  (82) *"answers moved" is resolved by a dt SWEEP, not by inspection — and
  my proposed dichotomy was false.* I asked whether the delta was
  second-order (expected) or first-order (a sign/factor error). Neither:
  local `O((k·dt)²)` accumulated over `t/dt` steps **is first order
  globally**. The real discriminator is whether the gap shrinks with `dt` —
  measured 3.04e-2 → 1.44e-2 → 6.87e-3 → 2.22e-3 at dt 60/30/15/5, halving
  with the step. A gap that did **not** shrink would have been the error.
  Both schemes converge to the same limit and relaxation is nearer it at
  every step size. Moved: H2's storage pool +4.5e-3 °C, subarea decks
  0.05–0.15 °C.
  (83) *a guard can be SHADOWED by a later redundant check, making its
  falsifier a no-op.* `sign(k) ≡ sign(J′)` given the earlier positivity
  checks, so `if (!(k > 0.0))` shadows the named `J′ > 0` guard and dropping
  only the first line changes nothing. Falsifier ii was underspecified: the
  observable variant is "remove **both**", which does fail gate 5. The line
  is redundant, not the guard.
  (84) *some failure modes are unconstructible from a deck — which is
  exactly why they hide.* Falsifier vi needed a node at ~1e-5 m depth (the
  router floors depth at 1e-4 ft) or `ROUTING_STEP 3600` (DYNWAVE's variable
  step overrides it); both routes landed at equilibrium under either scheme
  (−27.4445 vs −27.4413). **The 1D node/link bindings cannot reach the
  divergence regime from any deck**, which is precisely why it sat unnoticed
  in H2/H3 and only surfaced when H5a introduced subarea films. An
  unreachable-from-a-deck hazard needs a unit gate or it has no witness at
  all.
  (85) *removing an export must sweep PROSE, not just calls.* `deltaT`'s
  deletion left four by-name references, including one **in
  `SurfaceExchange.hpp` describing an export that no longer existed**.
  *Also recorded, in the engine's favour:* deleting `kMaxStepC` put
  everything on `J′ > 0`, and the validator **swept it rather than reasoning
  about it** — 3,135,820 samples over `Tw ∈ [−40,60]`, `Tair ∈ [−40,50]`,
  `RH ∈ [1,100]`, `wind ∈ [0,12]`: `J′ > 0` everywhere, for surface,
  radiative and combined. The anti-damping fallback is dead code in
  practice, which is what makes §4.2 safe rather than merely plausible.
  *Sweep:* 7 of 9 observed. Falsifier iii escaped as delivered (at
  `k·dt ≈ 4e-7`, `exp(x)−1` is accurate to ~1e-10 against a 1e-6 band —
  four orders inside) and is now closed by sweeping `dt` down to 1e-14 where
  the subtraction cancels to a hard zero.
- **D-H5e round (`c292b8eb`) — the merge landed; my §6 was wrong in a way
  worth more than the merge.**
  (86) *before concluding a failure mode is UNCONSTRUCTIBLE, check whether
  the limiting quantity depends on CONFIGURATION rather than on the
  binding.* I predicted the defect falsifier would escape, citing D-H5d's
  finding that a 1D node cannot be driven to large `k·dt`. That finding was
  measured with **radiative alone**, where `J′ ≈ 5.5` puts the required
  depth under the router's 1e-4 ft floor. **`J′` is a property of which
  families are enabled, not of the binding.** Summing the surface family in
  at 20 mph and 20 % RH gives `J′ = 45.9` — eight times larger, so the
  required depth rises by the same factor to 2e-4 ft, **twice the floor, on
  an ordinary storage node**. `k·dt = 1.80`. The falsifier was constructible
  all along. This is the exact inverse of lesson 84, and it was my own
  conclusion from the previous round applied one generalization too far.
  (87) *when a falsifier escapes, look for the ENCLOSING guard before
  concluding the behaviour is unobserved.* Falsifier iii — make
  `surfaceFluxOut` ignore its own toggle — failed nothing because
  `applyHeatFluxes` returns early when **both** toggles are off, so an
  all-off deck never reaches the evaluator, and the only mixed-toggle deck
  (H3's pool) asserts a direction a spurious latent term does not reverse.
  **Second appearance** after D-H5d's shadowed `J′ > 0` line; this is a
  family now, not an incident. Closed by a surface-off/radiative-on leg on
  gate 8.
  (88) *a falsifier that cannot be EXPRESSED is stronger evidence than one
  that is inert.* §4.2's claim was that reverting either shared-`netFluxOut`
  lambda would change nothing. It is stronger: as first written it **does not
  compile**, because the changeset also removed the `RadiativeExchange.hpp`
  include from both files. The include had to be restored before the old
  shape could even be written down. Behaviour-preservation verified rather
  than assumed, and the duplication is now structurally unavailable rather
  than discouraged.
  (89) *a standing exemption must be RE-VERIFIED, not inherited.* The
  "pre-existing bistable FV mesh-convergence failure" **is gone** — it passes
  at base, because foreign commit `71829e14 WIP` rewrote
  `test_fv_engine_integration.cpp`. It had been carried verbatim into every
  handoff for six rounds as the expected standing failure, and would have
  been carried into the next one. Suite is **153/153**.
  *Gate 8, and the property it establishes:* a node whose depth never changes
  must sit where the SUMMED flux vanishes — no reference value. Merged:
  0.000 °C away. Sequential: **2.44 °C**. On that deck the surface-only
  equilibrium is +2.2364, radiative-only −30.1352, combined −0.3942 — and the
  split parks at **−2.8384, which is none of the three**. Both §5(a)
  discriminators came out clean: single-family decks are bit-identical
  (delta exactly `0.0`), and the `dt` sweep reads −2.444 / −1.136 / −0.432 /
  −0.216 at dt 10/5/2/1, halving with the step. **And the merged answer is
  `dt`-INDEPENDENT** (−0.3942384229 at every step): exact at steady state for
  any timestep, while the split's answer is a function of the timestep.
- **~~⬜ DECISION OWED — merge the node/link flux bindings~~ — RESOLVED as
  D-H5e (`c292b8eb`).** Kept for the record: Three of four bindings already sum every enabled module into
  a single `netFluxOut` before relaxing (`ArdEngine.cpp:1163` cells,
  `HeatWatershed.cpp` subareas). Only the LEGACY node/link path relaxes
  twice, at `HeatLegacy.cpp:101-102`. The merge also collapses the node and
  link geometry traversal that `SurfaceExchange.cpp` and
  `RadiativeExchange.cpp` currently duplicate. **Resolve before H6 adds a
  fifth flux family.**
- **H5b round (`1c78e9dd`) — six lessons, and the first is the worst gate
  defect of the program.**
  (90) *a declaration is not a value — SECOND instance, on the parameters a
  plan section I wrote had named.* D-H5b told the implementer to follow
  `GWComponent` and quoted `sedDensity = 2650`, `sedCp = 880` from
  `gwmodel.h:870-871`. Those are **dead in-class initializers**; the
  constructor overrides both (`gwmodel.cpp:51-52`, 1970 / 2758) and effective
  `ρ·cp` differs by **2.3×**. Caught before any code, by reading the writes.
  (91) *a gate's NAME and its doc header can both assert a property the gate
  does not compute.* `ConductionConservesTheColumnsHeatContent` asserted
  finiteness, bracketing and `cap_total > 0` — **it never computed
  `Σ cap·T`**. The file header repeated the claim. So falsifier iii
  (asymmetric inter-layer flux) **passed everything while manufacturing
  3.7 % of the column's energy** — 49.87 → 51.74 MJ/m², against the correct
  form's −0.014 % drift. I had exported `lidLayerHeatCapacity` *specifically*
  so the gate could compute that ledger, and then did not use it for the
  ledger. The name, the docstring and the helper all said it was done.
  **Every conservation gate must be read for what it ARITHMETICALLY does,
  never for what it is called.**
  (92) *a falsifier can escape for two INDEPENDENT reasons in sequence —
  after fixing one, re-run it.* Falsifier iv (revert `ensureSized` to
  `resize`) escaped first because every gate ran a single capability, and
  `resize` sizes correctly then; `Opts::water_age` defaulted to false. After
  a both-on leg was added it escaped **again**, because `initLidLayerAge`
  seeds from `INITIAL_STATE`, the age config had no such row, and the wipe
  replaced a zero seed with zero. Only a non-zero seed plus a SETUP assertion
  that it is non-zero catches it. Real damage once visible: SOIL age
  17842.60 → 17681.34 s, drain 17917.45 → 17750.90, runoff 16453.76 →
  16337.80.
  (93) *a static state variable does not mean a closed system.* The
  validator's first energy ledger read a 12 % loss that was really the
  outfall's `RouteTo` re-feeding the column at constant volume — `vol_prev`
  unchanged throughout. Constant state ≠ no flow, and a conservation ledger
  must close the boundary explicitly (storm stopped, drain coefficient
  zeroed, `RouteTo` removed) rather than infer closure from a stationary
  variable.
  (94) *a gate whose premise a later phase RETIRES must be inverted, not
  deleted.* H5a's contributor gate asserted `runon_inflow > known_rate`
  because the LID drain was uncounted. H5b counts it, so the two coincide
  exactly and the premise is dead. Inverted into the completeness invariant
  it was always reaching for — **every cfs of run-on has a known
  temperature** — it immediately caught falsifier vii, which I had predicted
  would escape. **And with all three contributors counted, A3's
  divide-by-the-total defect is now UNREPRESENTABLE rather than merely
  absent.**
  (95) *some compositions cannot be gated at all without an external
  reference, and saying so is the finding.* Falsifier ii (conduction as a
  separate pass) is **not** D-H5e's shape: conduction's fixed point (uniform)
  and the atmospheric one **coincide**, and both compositions are
  first-order consistent with the same ODE — so a `dt` refinement converges
  for both. The apparently decisive t=120 signature (sequential surface and
  soil within 0.004 °C) was an artifact: that layer is dry, capacity 0, and
  the value was a stale `HOLD` leftover. Measured rather than accepted from
  my prediction.
  *(85, second instance)* the `until H5` prose sweep was **not** empty — a
  stale claim survived at `HeatWatershed.cpp:166` calling the underdrain
  uncounted. A by-name sweep has to include the sentences, not only the
  symbols; this is the second round running that it did not.
  *Also:* §6's most dangerous outcome came back clean **with values, not a
  pass count** — every LID layer age, drain age and subcatchment runoff age
  across all six A4 decks, base vs H5b, differs only in the header line
  `species=1` → `species=2`. `layer_index` is correct under the widened
  stride. Sweep 8 of 9, from 5. Suite **154/154**.
- **H5b follow-up (`815f0e8e`) — the fix was right and the gate was vacuous.**
  (96) *zero depth is not absence of flow — lesson 93 one level down, and
  this time it made a whole gate vacuous rather than a ledger wrong.*
  `live[k]` is `v_old > tiny || v_in > tiny` — **depth OR inflow** — and my
  SETUP 2 asserted only depth. Storage depth reached exactly 0 while
  `in_stor` sat at **9.92e-7 ft/s, six orders above the 1e-12 threshold**,
  because the subcatchment trickles in long after the storm stops. So the
  branch the gate was about **was never entered**, and every one of
  falsifiers i–iv was inert on arrival — including the two I had predicted
  gate 9 would catch. My §2 claim that "storage drains to exactly zero" was
  right about `stor_depth` and **wrong about `live[storage]`**, which is the
  predicate that matters. **A SETUP assertion must assert every term of the
  predicate it is trying to reach**, not the term that is easiest to observe.
  §5(a)'s advice — lengthen the run, raise the drain — could not have worked:
  neither helps when the inflow never stops. `FromImp = 0` is what closes it,
  and rain barrel and infiltration trench show the same trickle (2.07e-7,
  2.38e-7), so it is the **deck knob, not the LID type**.
  (97) *one assertion sees one half; a gate about a two-consequence defect
  needs one leg per consequence.* Three were required. **Not-reset** catches
  falsifier ii, but only against a margin: the policy value enters the
  solve's right-hand side and conduction pulls it partway back within the
  step, so the reset lands at **20.004** and a bare `EXPECT_NE(storage,
  20.0)` passes on the defect. **Still-in-the-solve** catches i as *movement
  between two end times* (28.31646534 → 28.33914633 coupled, bit-identical
  excluded — a dropped layer is frozen from the moment it dries).
  **Dry-surface-keeps-the-policy** catches iii and iv.
  (98) *a crash in the probe is not a crash in the engine, and the backtrace
  names the probe.* Three decks appeared to segfault; the fault was in the
  validator's own instrumentation — `lid().group(0)` is the bio-cell group,
  empty for other LID types. Nearly reported as an engine defect. When
  instrumentation crashes, suspect the instrumentation first.
  *Scored:* §6 predicted falsifier **iii would escape** and it does not — and
  the mechanism is exactly what §4.2 argued: uniform `mass[]` gives a dry
  surface an invented capacity and it equilibrates to **29.54 against 20**.
  The reasoning that motivated the asymmetry is what the falsifier confirms.
  Falsifier iv's unverified assumption is confirmed too: `pave_thick = 0` for
  a bioretention cell. Sweep **0 of 4 → 4 of 4**. No gate moved (§5(c)): on
  the default deck family the branch is never entered, so the changeset is
  inert there — consistent with deck and age identity.
- **~~⬜ ACTION OWED (H5b) — a dry-but-present layer drops OUT of the
  conduction system~~ — RESOLVED in `815f0e8e`**, together with a second
  consequence §9.8 had not named: `live[k]` also gated the D-H5c policy, so a
  drained layer was **reset every step** to a constant. Both sites now use
  `mass[k]`, and the gate pins both directions — a present-but-dry layer
  governed by conduction, a dry surface layer governed by the policy. Kept
  for the record: `HeatLid.cpp:303` builds the tridiagonal index set
  from `live[k] && thick[k] > 0.0`, and `live[k]` requires water. A drying
  soil layer therefore leaves a wet surface and a wet storage layer
  **conducting directly across the gap it should insulate**. Reachable during
  ordinary drying, observed by nothing. The fix is to include any layer with
  `thick > 0` in the conduction system while keeping `live` for advection —
  conduction crosses the matrix, which is present whether or not it holds
  water, and that distinction is already drawn in `layerThickness`'s own
  docstring.
- **`dt`-refinement instrument (`0e8e57df`) — FOUR carried falsifiers closed
  at once. Sweep 5 of 5, suite 155/155, 39–46 ms for the whole binary.**

  | gate | observable | correct | ratio | defective | sep | band |
  |---|---|---|---|---|---|---|
  | 1 | LID storage age | 0.000747 | 3.25 | 0.002268 | 3.04× | 0.0012 |
  | 2 | subarea temp | 0.008761 | 2.68 | 0.024273 | 2.77× | 0.014 |
  | 3 | node temp | 0.017842 | 2.66 | **NaN** | ∞ | 0.030 |
  | 4a | LID storage temp | 0.000650 | 3.62 | 0.002237 | 3.44× | 0.0011 |
  | 4b | LID soil temp | 0.016085 | 3.30 | 0.037097 | 2.31× | 0.023 |

  (99) *a scale-relative band still needs the scale and the observable in the
  SAME units.* `sourceSpread` returned the age spread in **hours** while
  `subcatch_runoff_age` publishes **seconds** — so gate 1's band was 3600×
  tighter than it read (248.8 against 0.05) and the gate failed in its
  correct form while blind to its own falsifier. §4.2's whole argument for
  scale-relative bands was that they cannot carry a stale magnitude; that is
  true and it does not protect against carrying the wrong *unit*.
  (100) *a gate can measure the wrong SUBSYSTEM's timestep sensitivity.* Both
  LID gates read a storage layer sitting at 3.06e-4 ft of a 0.75 ft capacity
  — **0.04 %, the noise floor** — whose volume is not even monotone in `dt`
  (3.116e-4 / 3.232e-4 / 3.058e-4). They were measuring the LID solver's own
  step sensitivity, not the transport scheme's. One knob, `drain_coeff`,
  moved the separations **1.00× → 3.04×** and **1.49× → 3.44×**. Lesson 55's
  shape again: find the regime where the term under test *dominates*.
  (101) *leg (2) is ONE-SIDED, and a defect whose sign opposes the
  discretization error reads as BETTER convergence.* Worked example: gate 1
  at `drain_coeff = 1.0e-4`, correct **0.001362**, defective **0.000292**. A
  two-sided form would need a reference value — the thing the instrument
  exists to avoid. **Recorded so a future round does not "improve" this into
  two-sidedness and lose the property.**
  (102) *the contraction ratio is reported, not asserted — and my file
  claimed it was checked when nothing checked it.* Measured: a **no-LID** deck
  moves node depth 0.10 % coarse→fine and contracts at **4.32** — those gates
  measure the scheme. A **LID** deck moves 4.83 % and contracts at **2.76**,
  which is why every LID ratio sits at 2.4–3.6. Pinning the ratio would gate
  the hydraulics. Validity condition established instead: **the falsifiers
  leave every depth and volume bit-identical**, so each separation above is
  measured at a *fixed flow solution*.
  (103) *the gates are not independent, and that is recorded rather than
  fixed.* Falsifier ii trips gate 3 as well as gate 2 (0.0433) — that node's
  water *is* the subcatchment's runoff. In the other direction falsifier iii
  is invisible to gate 2 (7.7e-08). And iii does not converge slowly, it
  **diverges**: NaN at the mid and fine levels, H5a's forward-Euler failure
  with the 5 °C refusal removed.
  *On §4.3:* the validator kept the single deck family and this round is the
  argument for it — **one knob on the shared writer fixed both broken gates
  at once.** §4(a) never fired; §4.1's ladder was never the problem.
- **⚠⚠ S1 round (`274b6506` + `d7ee70be`) — the transport defect was the
  SECOND of two stacked gaps, and the first is an engine defect that predates
  the whole program.**

  **`SnowSolver::setMeltCoeffs` had no caller anywhere in `src/engine/`.**
  `dhm` stayed at its `assign(0.0)` value, so `imelt = dhm·(temp − tbase)` was
  **identically zero on every deck** and **degree-day snowmelt has never fired
  in this engine** — only rain-on-snow, which needs 0.02 in/hr. Legacy calls
  it once a day from `setTemp` (`climate.c:1176-1180`). Landed first and
  separately as `274b6506`, with an engine-level gate in `test_snow.cpp`;
  155/155 with that commit alone, verified independently.

  (104) *a function can be fully unit-tested and never called by the engine —
  the tests exercised the FUNCTION, not the WIRING.* `test_snow.cpp` has 35
  gates and **every one of them calls `setMeltCoeffs` itself** before stepping
  the solver. The coverage was real and the engine's use of it was never
  observed once. Distinct from lesson 52 (a stage guard that is not
  `loadersNeeded`): there a guard was missing, here the **call** was missing
  and the tests supplied it themselves. **A unit suite that constructs its own
  preconditions cannot see a missing caller.**
  (105) *a defect can survive because the condition that would expose it never
  OCCURS, not merely because no deck asks for it.* S1's defect survived A3,
  H5a, A4 and H5b through **two stacked gaps**: no gate used a `[SNOWPACKS]`
  deck, and even with one there would have been no melt to expose it.
  Removing either alone would have left it invisible.
  (106) *a "negative" deck can be negative for a DIFFERENT reason than
  intended.* Gate 5 was vacuous: a pack-less **project** has
  `IGNORE_SNOWMELT` forced on (`PostParseResolver.cpp:2199`, legacy
  `project.c:221`), so the deck returned at the first guard and never reached
  the per-subcatchment guard the gate is named for. Falsifier iv escaped for
  the same reason. The absence you engineered may be produced by a mechanism
  upstream of the one you are testing. Gate 5 now runs two subcatchments on a
  **pack-bearing** project; gate 6 gained a runtime-raised-flag leg
  (`openswmm_model_impl.cpp:1189`).
  (107) *my "probably nothing" on a sentinel distinction was wrong for the
  second round running.* Falsifier iii **is** constructible and is now gate 7:
  below `tbase` nothing melts, and under full cover no rain reaches the
  ground, so `imelt + rain·(1 − asc)` is **exactly 0.0** while the gage reads
  1.157e-05 — a pack absorbing every drop. `>= 0.0` returns the genuine zero;
  `> 0.0` hands the surface rain that went into the snow.
  (108) *a redundant guard that mirrors an upstream contract is
  documentation, not dead code.* Falsifier v **cannot** be made to fail:
  `snow_net_*` are written only for pack-bearing subcatchments and initialise
  to −1.0, so the sentinel already answers "no pack". The guard was kept
  because it mirrors the solver's own test, with the redundancy stated in the
  code rather than left for a reader to rediscover.

  **The magnitude, measured (§6.6) — hydrology bit-identical either way
  (2937.651904 ft³ of runoff):**

  | observable | correct (S1) | pre-S1 |
  |---|---|---|
  | subarea age IMPERV0 / IMPERV1 / PERV | 624.67 / 787.37 / 0 s | **3600 / 3600 / 3600 s** |
  | runoff age | 753.75 s | **3600 s** |
  | subarea temperature | 0 °C | **25 °C** |

  **3600 s is the elapsed run time exactly** — what a surface reads when
  nothing mixes into it. A3's net-gain failure through a different field, now
  a number rather than an argument. Gate 3 was checked against three controls
  (25→25, 10→10, no-pack holds 25) so it is not passing on an unseeded field.

  *Also found:* melt does not leave a pack until free water exceeds
  `fwfrac·wsnow` — 0.05 ft here, ~98 min to fill on a 60-min deck — so the
  deck seeds `fw0 = fwfrac·sd0` and starts RIPE. `sd0` converts correctly
  (6 in → 0.5 ft); **issue #131 was not in play.** Sweep 5 of 6; suite
  **156/156**.
- **S2a round (`8b7d1cf7`) — the changeset segfaulted on its first gate, and
  the cause was a field added in one of SIX places.**
  (109) *adding a field to a hand-enumerated SoA needs every enumeration
  site, and the count is NOT discoverable from the one you edited.*
  `SubcatchData` lists its arrays by hand in **six** functions — `resize`,
  `grow`, `reserve`, `erase`, `shrink_to_fit`, `reset_state`. I added
  `snow_melt_*` to `resize()` only, because that is where `snow_net_*`'s
  `assign` lives and **a correct-looking sibling right there made it look
  complete**. The parser adds subcatchments through `grow()`, so the new pair
  stayed size 0 while `snow_net_*` grew, and the publication wrote past the
  end: `EXC_BAD_ACCESS at 0x0` in `stepRunoff`. **"I added it beside its
  sibling" is not "I added it everywhere its sibling appears."** The standing
  check is now: `grep -n "<sibling_field>" <the SoA header>` and match the
  count, before compiling.
  (110) *predicting a symptom is not diagnosing it, and my remedy was right
  by accident.* §5.3(a) called gate 9's skip correctly and blamed the deck.
  The real cause is two engine defaults conspiring: the default ADC curve is
  **all ones** (`Snow.hpp:92`) *and* `si` is pinned to the initial pack depth
  because **the deck's `SD100` field is never read**
  (`SWMMEngine.cpp:5613`). So every snow deck in this program sits at
  `asc = 1`, `rain·(1 − asc)` is identically zero, and **no rain has ever
  reached the ground under a pack**. The suggested fix (an explicit ADC row)
  happens to work — cover 0.5 at 0.2 in/hr gives `f = 0.4306` — but the
  handoff would have sent someone hunting in the wrong place.
  (111) *a non-strict bracket is satisfied by its own endpoints, and one of
  those endpoints was the defect.* Gate 9 asserted `min ≤ t ≤ max` where
  `min` is the meltwater temperature and `max` the configured rain value —
  **the S1-only answer sits exactly on `max`**, so the gate passed on the
  defect it was written for. Lesson 91's family, and mine again. Tightened to
  strict; it now catches falsifier i as well.
  *Sweep 4 of 5.* Falsifier **ii** closed by a new gate 11, which needs
  `snn0 > 0` **and** partial cover at once — the plowable surface is never
  depleted (`Snow.cpp:100`) while the others are, and that is what makes the
  two melt rates differ at all. Published `2.450428683e-06` sits strictly
  between the surface rates `3.501e-06` and `1.750e-06`; reading either raw
  lands on an endpoint. Falsifier **v** does **not** escape — H5a's own suite
  catches it, and §4.3's reasoning (run-on is not precipitation) held. The
  `[0,1]` clamp is the only real escape, which §4.2 said outright.

  **The magnitude, measured — hydrology identical either way:**

  | deck | observable | S2a | S1-only |
  |---|---|---|---|
  | pure melt | arriving | **0 °C** | 20 °C |
  | | subareas | 0 / 0 / 0 | 20 / 20 / 20 |
  | melt + rain-through | arriving | **7.235 / 7.235 / 11.389 °C** | 20 / 20 / 20 |
  | | runoff | 7.195 | 20 |

  **The per-subarea spread in the second deck is something the S1 form cannot
  produce at all** — one scalar cannot differ between surfaces.
- **S3 round (`c316c83e`) — THREE OF THE FOUR GATES PASSED WITH THEIR OWN
  DEFECT FULLY RESTORED, and one cause explains all three.**
  (112) *a fixture chosen to make a phenomenon visible can make the defect
  invisible.* The shared deck writer starts every pack **RIPE**
  (`fw0 = fwfrac·sd0`, exactly the capacity) — added in S1 so that melt would
  leave a pack at all. A store already at capacity drains every drop of melt
  the instant it appears, so `excess == vmelt` and "SWE −= melt" and
  "SWE −= excess" are **the same number**. Gate 12 (F2) could not fail.
  Gate 13 (F3) was blind twice over: `fw` starts at 0.45 ft so `fw > 0` is
  the *initial condition*, and at 40 °F the store fills with meltwater
  regardless of rain. It now runs unripe and **below freezing**, with a SETUP
  asserting SWE did not move.
  (113) *a SETUP can be arithmetically wrong and still look like a finding.*
  Gate 12's SETUP compared the pack against `sd0` alone while a ripe pack is
  *given* `sd0 + fwfrac·sd0` — at `fwfrac = 0.90` that reads 0.937 ft against
  a 0.5 ft ceiling and **reports mass creation that is not there.** Gate 15
  had the right expression all along.
  (114) *"unclosable" is a claim about the fixtures on hand, not about the
  defect.* §5.5 called falsifier iii unclosable without "a deck poised exactly
  at capacity" — **which is what a ripe pack is, and the round already had
  one.** Measured on gate 15's deck: **2148.145 ft³ of runoff against
  2125.994**, a 1.0 % retiming. New gate 15b states the invariant it breaks —
  free water can never exceed its *current* capacity, because a shrinking pack
  must give up what it no longer has the snow to hold.
  *Sweep 5 of 5.* Suite **158/158**, 14/14 decks byte-identical, 69 tests
  ASan-clean. An S1 gate moved correctly: under S3 a ripe pack **transmits**
  rain rather than absorbing it, so `snow_net` reads the whole gage rate; its
  deck now starts unripe. F2's signature, measured — unripe pack, 40 °F, one
  hour, no precipitation: SWE **0.48642 ft** under S3 against **exactly 0.5**
  before, with `runoff_vol` 0 in both. *That is the regime the defect lived
  in: no output to look at, and a pack that never depletes.*
- **O4 return-code round (`97bfa512`) — MY CORRECTION WAS HALF WRONG, IN THE
  HALF IT WAS WRITTEN TO FIX.**
  (126) *a correction is a claim, and it inherits the burden it was written to
  discharge.* I said `report()` before `end()` is refused with
  `SWMM_ERR_WRONG_STATE`, naming the guard at `SWMMEngine.cpp:4924` **from a
  code read, without running it**. That guard never fires: `step()` sets
  `ENDED` when the simulation finishes (`:1177`), so by the time the driver
  reports the engine is *already* ended — `report()` **succeeds** and sets
  `REPORTED` (`:4970`), and it is **`end()`** that is refused, accepting only
  `RUNNING` or `ENDED` (`:4813`) and returning code 6. The predicted line
  never appeared; the measured one was
  `end() returned 6: swmm_engine_end: engine must be running or ended`.
  My "a report that never ran" is refuted by the artefact — `o4_reportfirst.rpt`
  is a **complete 204-line report**, same length as the control, carrying the
  whole continuity block and `Final Snow Cover 0.340`.
  **The half that stands is the round's point:** there *is* a return code and
  the instrument discarded it (lesson 124). I wrote §5.5 as *"I would rather
  it were tested than believed"* — and it was, and it disproved me. That is
  the process working, not failing.
  *Falsifier **ii-b**, which the round had to ADD because my ii aimed at the
  wrong guard, settles the artefacts:* letting `end()` accept `REPORTED`
  closes the `.out` (190446 → 190470, byte-identical to `cli`) and leaves the
  `.rpt` unchanged. **So the `.out` truncation is `end()`'s absence and the
  `.rpt` omissions are a report that ran early — both explanations were
  partly right.** The original round was right about the report and wrong
  about the error code; my correction was right about the error code and
  wrong about the report.
  (127) *two test files that write the same fixture name into a shared working
  directory are ONE TEST, and the failure wears a disguise.* `-j8` failed
  `test_engine_heat_watershed`: it and `test_engine_heat_lid` **both wrote
  `_h5b.inp` / `_h5b.heat`** into the shared `data/` cwd. 8 failures in 8
  concurrent runs, every one `SURFACE_EXCHANGE did not parse` — **a content
  error, not a file conflict**, which is why S2b's round wrote the sibling
  defect off as a flake. **Both `_h5b` names are mine**, in two files I wrote
  a round apart. *A shared-fixture race corrupts whatever count is being
  quoted that round, not just its own suite* — which is why the round fixed
  it rather than reporting it, and that was the right call.
- **⬜ OWED (C API contract) — `end()` after `report()` leaves the `.out`
  unfinalised and nothing forces the caller to notice.** It returns
  `SWMM_ERR_WRONG_STATE`, but a caller that ignores it (as my driver did)
  gets a truncated results file with no other signal. Either the state
  machine should let `end()` close a `REPORTED` run — measured by ii-b to be
  sufficient — or the truncation should be documented. Recorded, not fixed.
- **O4 differential round (`3bdc30a2`) — OUTCOME A, and the control passes so
  it counts.** `o4_cli.out` is byte-identical to a real `openswmm` run of the
  parity deck; the `.rpt` matches with timestamps excluded. **All five
  variants: 8,640 steps, continuity 0.407 %, snow rows 1.500 / 0.340 —
  every hydrology number identical.** O4's signature is 7.25 in against
  12.98 in delivered to the ground, and **nothing in the C API moves by a
  thousandth.** The C API is exonerated; **the MCP server is the variable
  left standing**, and that re-run needs a session that has the openswmm MCP
  tools.
  (124) *an instrument that discards a return value cannot tell "the engine
  ALLOWED this" from "the engine REFUSED and nobody asked."* My §7(b)
  predicted `report()` before `end()` "may be illegal"; the round recorded it
  as **"legal and silently lossy — no crash, no error code."** Both are
  wrong — **and so was this correction, which is the part worth keeping.**
  There IS an error code and the driver threw it away, on both branches; that
  much stands, and it is lesson 91's family applied to the **instrument**
  rather than to a gate. But the refused call is not `report()`.
  **MEASURED once the codes were printed:** `step()` sets `ENDED` at the end
  of the run (`SWMMEngine.cpp:1177`), so `report()`'s `state != ENDED` guard
  (4924) never fires — `report()` **succeeds** and sets `REPORTED` (4970),
  and the `.rpt` is a complete 204-line report carrying the whole continuity
  block. **`end()` is what the engine refuses** (4813 accepts only
  `RUNNING`/`ENDED`), returning code 6. Falsifier ii — removing `report()`'s
  guard outright — changes **nothing at all**, which is what settled it.
  Falsifier ii-b — letting `end()` accept `REPORTED` — separates the two
  artefacts: the `.out` closes and becomes byte-identical to `cli`, while the
  `.rpt`'s `Link C1 (0)` and real timestep ladder stay missing, because
  `report()` had already written the file before `end()` computed them. So
  **the `.out` is `end()`'s absence and the `.rpt` IS a report that ran
  early** — the original wording was right about the report and wrong about
  the error code; the correction was right about the error code and wrong
  about the report.
  (126) *a correction is a claim and inherits the burden it was written to
  discharge. This one named a guard from a code read, without running the
  thing — and the guard it named never fires.*
  (125) *a protocol that names two binaries assumes they are CO-LOCATED, and
  a build that does not put them there makes the instructions wrong in a way
  that only bites the person following them.* Without `POST_BUILD` staging
  the driver sits in its own source directory while `openswmm` is in
  `bin/<config>/`, and §3's and §4's own commands read as though the two are
  together. My omission; the round added the CLI's staging (and its Windows
  DLL copy, untested), deliberately **not** its install/RPATH/bundling rules —
  an off-by-default instrument is not a shipped artefact.
  *Answered as written:* §7(a) did not happen — the option wired up first try
  and an ordinary build is unaffected. §7(c) — `close()` is a real teardown
  (`SWMMEngine.cpp:4977`: perf dump, IO thread stop, interface/RDII files,
  `EngineState::CLOSED`), so `reopened` tests what it claims; **stated
  positively, open/close/open on one handle leaves no residue, byte for
  byte.** §7(d) — the absolute prefix held, and a second full run from a
  different binary location reproduced all five files exactly, so nothing
  read a stale file. `nosave` costs the results file and nothing else: a
  390-byte header and a byte-identical report.
- **⚠ LESSON-NUMBER COLLISION, resolved here.** Two rounds written in
  parallel both assigned **(119)**. Canonical, chronological by commit:
  **118** and **119** are the S2b round's (`2a58d82c`); **120** is the
  continuity finding's, renumbered in `SNOW_CONTINUITY_FINDING_2026-08-21.md`
  and the register. The F8/F9 report calls the denominator lesson (120); it
  is canonically **121**. *A register whose numbers are ambiguous is not a
  register* — this roadmap is the authority and the docs now agree with it.
- **S2b round (`2a58d82c`) — lessons carried in
  `S2B_PACK_AGE_HANDOFF_2026-08-21.md`; consolidated here.**
  (118) *a unit that lives only in an IDENTIFIER is not carried by the
  compiler, so finding one instance of the slip obliges a sweep of every use
  — and the instances that still PASS are the dangerous ones, because they
  pass for a reason unrelated to what they test.*
  (119) *a gate retired by a fix and re-aimed at a new premise is a NEW gate,
  and it owes the same falsifier the original one owed.* Two separate edits
  re-aimed gate 2 and corrected its arithmetic; **both were right and neither
  was a test.**
- **Continuity-finding round — consolidated here.**
  (120) *a table that contains its own refutation still reads as evidence.*
  Before ranking causes for a residual, check whether the residual **varies
  with the thing it is supposed to be made of** — if it does not, the
  measurement is what is broken, not the model.
- **F8/F9 round (`0ad28685`) — the ledger learns about snow. 159/160, 14/14
  decks byte-identical in BOTH `.out` and `.rpt`, sweep 7 of 7, 80 tests
  ASan/UBSan clean.** Continuity error **−8.193 % → +0.407 %**.
  (121) *a closure gate is only as good as its DENOMINATOR — an error
  expressed as a fraction of total input cannot see a term removed from the
  input side when that term IS the input.* Falsifier i escaped **both**
  ledger gates, past their SETUP legs: `runoff_error()` returns 0.0 when
  `total_in` is zero, and on a deck whose only input is the pack, dropping
  `runoff_init_snow` makes `total_in` exactly zero. **The gate was reading
  the fallback, not an error.** Falsifier vii failed on gate 26 alone for the
  same reason. Closed by putting 0.2 in/hr on both decks — at 10 °F gate 24's
  pack cannot melt and an hour is well inside its free-water capacity, so
  nothing leaves and the premise survives. Gate 24 now also catches falsifier
  iv, which it previously could not.
  (122) *a duplicated magic literal is a SECOND CALL SITE for a fix, and a
  fix list that names files does not find it.* F9's `43560.0` correction also
  failed `SnowRemoved.PlowingAccumulatesRemoved`, an existing gate carrying
  the identical literal — first ctest came back 158/160. **That check was not
  on my handoff's list and should have been.** Lesson 109's family (a
  hand-enumerated set), for a *constant* rather than an SoA field: the sweep
  is `grep` on the literal, not on the symbol.
  (123) *a prediction that misses can still be a CONFIRMATION, if the two
  routes are independent — so report the reconciliation, not the delta.* §1
  predicted **+1.419 %**; the ledger reads **+0.407 %**. The gap is a check:
  the prediction came from a hand reconciliation that carried `Snow Removed`
  as an *unquantified hypothesis* and read the pack from `newSnowDepth` (SWE
  only). The ledger supplies removal at **0.122 in on `SUB_DEEP`** — the one
  subcatchment with `Fout = 0.20`, exactly where the reconciliation had put
  it — and the pack **with** its free water at 0.340 in against 0.323 SWE.
  `0.1916 − 0.122 − 0.017 = 0.055`. **Two independent routes agreeing to the
  third decimal is what retires the "unexplained loss"** — not the new number
  by itself.
  *Two of my predictions were wrong and both usefully.* **§6(b):** the parity
  `.out` cannot move — F8 writes ledger fields and report rows, and
  `snowCoverVolumeFt3` reads state without touching it. The `.rpt` moved, and
  that is where the change lives. **§7(v):** F9 **is** observable without an
  SI deck — `plowSnow` takes a `SimulationContext`, so the unit system is a
  *field*, and the new leg builds the same pack under CFS and CMS and asserts
  the 2.471 ratio. On US decks the fix is not a no-op either:
  `1/Ucf[LANDAREA][US]` is 43561.596, so `removed` moves by 1.0000366 —
  below the report's three decimals, and correct, because legacy parses
  `Subcatch.area` with the same division.
  *Confirmed as written:* §5(1) ordering (`initHydrology` 5324 →
  `initMassBalance` 5327, SD0 at 5753); §5(2)/(3) by the compile; §5(4) zero
  `-Wswitch`; §6(c) gate 26's SETUP never fired; report row order matches
  `report.c:512-570` line for line, guard placement included.
  *The one ctest failure* is Track I's 0.31 % 2D infiltration re-derivation,
  **unchanged by this changeset**.
- **S4 round (`2992f7c5`) — FIXING ONE DEFECT IS WHAT EXPOSED THE NEXT, and
  the round's most valuable output is a RETRACTION.**
  (115) *a magic constant that looks wrong may be a calibration; check what it
  makes true before correcting it.* D1 — recorded for two rounds as an
  upstream EPA defect — was **this program's own error**. Legacy's
  `0.0172615` has a period of exactly 364 days; 364 = 4 × 91 puts the melt
  peak on day 172, the summer solstice, and makes the equinox-to-solstice
  quarter a whole number of days. Lesson 69's shape applied to a number rather
  than a field. **Every divergence this program has found in the snow module
  has been the engine's; none is reportable upstream.**
  (116) *a necessary fix is not a sufficient one, and only a gate on the
  OBSERVABLE will say so.* Reading the deck's `SD100` (**F6**) was necessary
  for areal depletion and did not move `asc` at all: `awe` initialises to 0
  where legacy uses 1.0 (`snow.c:199`), so `awesi >= awe` holds on the first
  depleting step and cover is pinned a second, independent way (**F7**). It
  was invisible for exactly the reason F6 describes — while `si` was pinned to
  the pack depth, `wsnow >= si` fired on step 1 and *that* branch sets
  `awe = 1.0` itself, so **a pack starting below its SD100 never takes it.**
  Gate 16 caught it because it asserted `asc < 1` and not the mechanism.
  (117) *a guard can be provably redundant, and saying so is worth more than
  leaving it ambiguous.* Falsifier iv dropped the `si <= 0` guard and nothing
  failed, where §6.4 expected most of the suite to crash. The condition is
  `si_val <= 0.0 || wsnow >= si_val`: when `si <= 0` the second test is
  already true for every non-negative `wsnow`, so the ADC branch is
  unreachable and there is no divide-by-zero to expose. Same shape as S1's
  falsifier v. Recorded in the register as redundant rather than load-bearing,
  because a redundant guard reads as load-bearing to the next person.
  *Sweep 3 of 5 plus a new one for F7.* Suite **158/158**, 14/14 decks
  byte-identical, 71 tests ASan-clean. Depletion needs a curve **and** a
  depth: the default ADC curve is all ones (`Snow.hpp:92`), so the two S2a
  gates that reached partial cover had been relying on the pinning for their
  `si` — §6.3(b) predicted that movement and attributed it to the wrong cause.
  Nothing else moved, so no deck was picking up the 7th column unintentionally.
  **§5.4(d)/§6.6 settled: none of the 14 reference decks has a `[SNOWPACKS]`
  section**, so the byte-identity across both rounds is a real result — and
  also the reason a snow parity deck is now owed (register §4).
- **~~⬜⬜ BLOCKING S2b — TWO HOLES IN THE SNOW WATER BALANCE~~ — ✅ CLOSED by
  the S3 round (`c316c83e`), and there were FOUR, not two.** Both entries
  stood: rain on the covered fraction was discarded (**F3**), and the
  instant-melt branch's water was assigned over and lost (**F5**). Reading
  legacy's `routeSnowmelt` to settle them found two more in the same eight
  lines — **F2**, SWE reduced by the drained excess rather than the melt,
  which is mass *creation* and meant a pack melting slower than its capacity
  never depleted at all; and **F4**, free-water capacity measured against
  pre-melt SWE, worth 1.0 % of runoff volume on a ripe melting deck.

  Settled the way this entry asked — as a fidelity question first. The answer
  was **not** "legacy does the same thing": legacy is right and the engine had
  diverged in all four places. Recorded in `SNOW_DIVERGENCE_REGISTER.md`.

  **S2b is unblocked.** Its complete-mix age model now rests on a balance that
  closes, which is the whole reason it was held.
- **~~⬜ OWED (parity) — `Snow.cpp:349` uses `sin(2π(day − 81)/365)` where
  legacy uses `0.0172615`~~ — ✅ DECIDED in S4 (`2992f7c5`), and the decision
  went the opposite way to this entry's framing.** This entry called
  `0.0172615` a defect to be matched or not; **it is a calibration, and ours
  was the error.** The period is exactly 364 days, so with the `day − 81`
  phase offset the melt peak lands on **day 172 — the summer solstice** —
  and the equinox-to-solstice quarter is a whole 91 days. `2π/365` peaks at
  172.25 and buys nothing. Reverted to legacy's constant, now named
  `kSeasonRad` with the calibration stated in the source. The one
  `test_snow.cpp` gate that had pinned the modern formula was asserting the
  departure, so **the gate was fixed, not the code**.

  Carried as a standing finding: **a magic constant that looks wrong may be a
  calibration — check what it makes true before correcting it.** Lesson 69's
  shape (a declaration is not a value) applied to a number rather than a
  field.
- **~~⬜ ACTION OWED (ops) — `.git/index.lock`~~ — CLEARED and recovered**
  during the S1 round; both `numstat` forms then agreed at `1  0`. HEAD also
  moved mid-round (foreign `962fd48c`, 2D bulk vertex-Z) with no file
  overlap, and the round **rebased and re-ran the suite, decks and sweep on
  the new base** rather than reporting from the older one. Kept for the
  record: The real index is stale against the new HEAD, so `git status`
  shows `tests/unit/engine/CMakeLists.txt` and
  `test_transport_dt_reference.cpp` as staged `D`/`M`. **The committed content
  and the worktree files are byte-identical** (`6a5e8dff…` both), so nothing
  is lost. The lock was **not** touched, per the standing constraint. When it
  clears: `git reset -q HEAD -- tests/unit/engine/CMakeLists.txt
  tests/unit/engine/test_transport_dt_reference.cpp` refreshes it. **Worth
  doing before any other session commits from that index** — this is the
  drift that produced the `f37f7dde`-round phantom deletions.
- **~~⬜ THE HIGHEST-VALUE OWED ITEM — a fine-`dt` external reference~~ —
  DELIVERED as `0e8e57df`.** Kept for the record: One instrument closes **four** carried items: A4's falsifier
  iii (mix-order), H5a's falsifier vi (flux applied to the post-mix volume),
  D-H5e's linearization caveat (long step vs many short ones), and H5b's
  falsifier ii (conduction composition), which the validator established
  cannot be discriminated any other way. Integrate one long step against N
  short ones and compare.
- **⬜ Owed gate (A4 falsifier iii) — the only one still escaping.** Reading
  the donor layer's **new** age instead of its **old** one (a mix-order error)
  moves the result by ~`dt` at steady state: **1.7 % against a 15 % band**. The
  validator left it owed **with the number** rather than tightening the band,
  which is right — observing a one-step ordering error needs a **transient**
  deck (a step change in inflow, where the donor's age is moving fast enough
  for one step to matter), not a narrower bound on a steady one. Same shape as
  the A2b falsifier ix/x carry.
- **⬜ Owed gate (H2 §6, now with a recipe):** a one-routing-step deck with a
  TRIANGULAR section so top width varies with depth, asserting the expected
  ΔT as a single `se::deltaT` call against an area read back from the
  context. That is the only shape that observes the top-width choice.
- **✅ CLOSED (ops) — the git index is accurate again.** The `f37f7dde`-round
  drift had grown to seven files reported as staged deletions (all five H1
  sources plus `test_output_species_ids.py`), every one present in both HEAD
  and the working tree, with `git diff HEAD` returning nonsense like
  `0+/145-` for files that had been edited. Fixed during the H2 round by
  checksumming all 184 listed paths, running a plain `git reset`, and
  confirming **not one byte changed**. Other sessions' work untouched and
  uncommitted.
- **~~⚠ ACTION OWED (ops, `f37f7dde` validation)~~ — RESOLVED above. The main git index had
  drifted** after several commits made through `GIT_INDEX_FILE`. `git status`
  shows `D  python/tests/engine/test_output_species_ids.py` **staged for
  deletion** — a file committed in `d7ce8efb`, present on disk and
  byte-identical to HEAD (5012 B, confirmed). Nothing is lost, but **a
  `git commit -a` by anyone would delete it**, and the same drift produces
  the spurious `MM` markers on `_output_reader.pyx/.pyi` and
  `tests/typing/test_surface.py`. Fix is a plain `git reset` (refreshes the
  index, stages nothing). **Deliberately NOT run by either agent**: it would
  also unstage anything a concurrent session has staged on purpose. Run it
  when no other session is mid-stage.
- **Unexplained, recorded because it was seen (`d7ce8efb` validation):** the
  first full Python run showed 3 extra failures in
  `tests/engine/test_files_iface_gaps.py` — which would have meant the new
  test file polluted a pre-existing one. It did **not** reproduce: two
  further full runs and an isolated run are clean and match base exactly. No
  explanation. If cross-test pollution ever surfaces in that file again,
  this is the first sighting.

- **✅ Fixture-collision round validated and COMMITTED `b85b802d`.** The
  guard configured clean first try and bites when made to, naming both source
  files; **159/160 at `-j8` three times and once at `-j1`**, the same Track I
  2D-infiltration failure each time. Two corrections to my handoff, both
  mine:
  - **Falsifier iii DOES reproduce, and `CONFIGURE_DEPENDS` is load-bearing.**
    §5 wrote it off as "nothing no falsifier here reproduces". One probe file
    does it: drop `CONFIGURE_DEPENDS`, add a test reusing `_h5b.inp`, build →
    **EXIT=0, the guard never runs and the collision ships**. Kept, it is
    EXIT=1 at the re-configure.
  - **My sweep figure was wrong in the part I did not measure.** §2 said "860
    literals in 25 files"; the directory holds **865 distinct literals across
    147 `test_*.cpp`**. The literal count was close; **the file count came
    from PROGRESS §3's table of *this program's* new test files and was never
    a count of the directory at all.** Coverage was complete either way — my
    sweep and the guard both glob all 147 and find no remaining shared
    literal — but a 25-file sweep would have been a partial one, and I
    reported a number I had not taken.
  - **`_out.inp` is prophylactic, not a caught defect.** Run the way ctest
    runs — one copy of each, 12 paired rounds — **0 failures in 24**. The
    "8 failures in 16" first reported came from a stress harness, and 8
    copies of `test_engine_object_deletion_ext` *alone* fail 4 of 8 on an
    unrelated `swmm_street_count` assertion: **the suite races itself.** That
    suite is unsafe against a second copy of itself; harmless under ctest,
    recorded, not fixed.
  - The `_h5b` finding is untouched by that: it was reported by a real
    `ctest -j8` before any harness existed, and `SURFACE_EXCHANGE did not
    parse` can only come from the LID suite's config.
- **(128) "no falsifier reproduces this" is a statement about the falsifiers
  that were written, not about the property.** A guard whose trigger is "a
  file is added" is tested by adding a file. I flagged the gap honestly and
  then treated flagging it as discharging it — §5's row even said "owed, and
  cheap to accept". It was cheap to *test*.
- **(129) N copies of one test binary is not a model of `ctest -jN`.** ctest
  runs each test once. A self-collision reproduces under a stress harness and
  never in the suite, and it reads as exactly the cross-suite race you went
  looking for. Corollary from the same round: **lead with the citation that
  came from the real runner.** The `_h5b` evidence was always the plain
  `ctest -j8`; quoting the harness's "8 in 8" put the weaker of the two
  citations in front.
- **✅ O4 RESOLVED (2026-08-22, run with the openswmm MCP tools) — it was a
  stale binary, not an execution path.** The MCP re-run reproduces O4 to the
  digit (3.674 / 3.573 in, +39.543 %). The `.out` parts from `o4_cli.out` at
  **period 120, 2026-01-06 01:00** — the first hour of the deck's first thaw
  — with header, IDs and properties byte-identical. Melt plotted against the
  deck's own forcing named the cause in one pass: **the API run melts only in
  the rain-on-snow window and never by degree-day — zero melt across eight
  consecutive days at 47 °F.** That is F1's signature (`setMeltCoeffs`
  uncalled, `dhm == 0`), fixed in `274b6506` on 08-20. The library the server
  loads was built **2026-08-03**. Nothing in the engine needs fixing.
  Account: `O4_RESOLVED_STALE_MCP_LIBRARY_2026-08-22.md`. **One link is
  unverified — the server's config was not read**, so the load path is
  inferred behaviourally; confirm before treating this as closed.
- **(130) an execution path and a build are different variables, and a
  differential that controls the first says nothing about the second.** O4
  held the deck, the commit and the entry point fixed and concluded the
  *path* must be responsible, because the path was all that was left in the
  frame. The binary was never in the frame. "Three CLI builds agree with each
  other and disagree with the API run" was read as controlling the commit
  range; it was equally evidence that **all three CLI builds were current and
  the API's was not**, and nobody asked the API's library how old it was.
  Corollary, from protocol §2: **a source read cannot eliminate a hypothesis
  about a binary.** §2 eliminated `setMeltCoeffs` by reading the code — right
  about the source, and the hypothesis was right about the mechanism and
  wrong only about whose copy.
- **(131) characterise the symptom before instrumenting the path.** The tell
  was free and available from the first measurement: **the API run melted
  snow only when it rained.** That is the physical signature of a specific
  known defect, and one plot of melt against the forcing found it in a
  minute. Four rounds went into instrumenting the execution path first.
  Lesson 110 says predicting a symptom is not diagnosing it; this is the
  converse — **the symptom was diagnostic and went unread.**
- **(132) a version string is not evidence about what code ran.** Two builds
  40 commits apart both report `6.0.0-alpha.3`; meanwhile
  `build/install-prefix/include/openswmm/version.h` is a **generated header
  dated 2026-06-01** still saying `alpha.2`, which is why `o4_cli.rpt` —
  produced from current source — prints alpha.2 while the tree has said
  alpha.3 since `d612283e`. Both defects are live. The header line nearly
  produced the exact wrong conclusion here: that the server had the *newer*
  binary, when it had one 17 days older.

- **⬜ IN VALIDATION — the bit-identity corpus was never in the tree
  (`CORPUS_IN_TREE_HANDOFF_2026-08-22.md`).** Every round of this program has
  reported "14/14 reference decks byte-identical". **`git ls-files
  tests/output` returns zero** — not one of those decks is tracked. They live
  in `tests/output/e0_validation_2026-08-16/decks/` and
  `tests/output/e2_validation_2026-08-16/`, two rounds' scratch directories
  from August 16, in a path that is not even gitignored. The runner exists as
  **ten-plus copies of `run_decks.sh`**, one per round directory, each
  hardcoding the repo root. `tests/parity/` — the snow deck, its generator
  and its baseline — is untracked as well. Changeset: the 14 decks copied
  byte-identical to `tests/parity/corpus/decks/`, a `MANIFEST` recording for
  each deck which path it reaches that no other deck reaches, one
  `run_corpus.sh`, and **snow_parity as entry 15** — closing
  `SNOW_DIVERGENCE_REGISTER.md` §4, a hard prerequisite since S2b landed.
  Before/after against two binaries, no stored baselines, not wired into
  ctest. Harness self-tested against stub binaries through eight probes
  (`tests/output/corpus_harness_selftest_2026-08-22/`); **not run against a
  real `openswmm`.**
- **The audit's second finding, recorded because it will keep mattering:**
  the corpus is **10 FV decks, 4 DYNWAVE, 0 KINWAVE (before the snow deck),
  0 STEADY** — two thirds of it is one router — and it has **0 water-age
  decks, 0 heat decks, 0 SI decks**. Not a design; what happens when decks
  are added by whoever needs one and nobody looks at the shape of the set.
  Now a table in `tests/parity/README.md` §4 rather than a caveat
  rediscovered in individual handoffs.
- **(133) a claim repeated every round is the least likely one to get
  checked.** "14/14 byte-identical" appeared in some fifty handoffs. It was
  true every time and it referred to files that were not in the repository,
  run by a script that was not in the repository, and nobody looked because
  the number never changed. **The stability of a figure is not evidence about
  its provenance** — it is what a figure looks like when the same untracked
  inputs are fed to the same untracked script.
- **(134) `cmp -l` counts differing byte positions and stops at the shorter
  file, so a pure length change counts zero.** My first `run_corpus.sh`
  printed `DIFFERS (0 bytes)` for a deck whose `.out` had grown a line — and
  a *truncated* `.out`, which is exactly what `reportfirst` produced two
  rounds ago, would have read the same way. Found by running the harness
  against stub binaries before shipping it. Sizes are now compared first.

- **✅ Corpus round validated and COMMITTED `d633c53e`** — 20 files. §4.1
  passes (15/15, exit 0); **§4.2 did not work as written, and that is the
  round.** Four findings, three of them mine:
  - **⛔ §4.2 compared one engine against itself.** `openswmm` is a thin
    driver over `@rpath/libopenswmm.engine.<v>.dylib`, so **a 143-line engine
    changeset leaves the CLI byte-identical** — both sides hashed
    `bdd99f49…`, and both then resolved the same dylib at load. **My runner
    hashed only the CLI**, so it printed `this run cannot fail` — accidentally
    true, diagnostically wrong, and silent in exactly the case it exists for,
    since on this project an engine-only changeset *always* produces
    identical CLIs. Fixed in the changeset: each side's engine library is
    resolved (beside the exe, `../lib`, `../engine`), both hashed into
    `PROVENANCE.txt`, and the vacuity note keys on the **library**. Verified
    three ways. README §6: two binaries means two build **directories**.
  - **⚠ The cost figure was ~50× out, in the direction nobody guessed.**
    `snow_parity` is **0.30 s** — among the *fastest* decks — where MANIFEST
    and `snow/README.md` called it "the slowest here by an order of
    magnitude, ~15–24 s". The three `sdm_fv_*` decks are **47.1 s of 49.3 s,
    96 %**; a full before/after run is ~100 s. The step count was right and
    the wall time **was never measured on a CLI build** — inherited from the
    same stale-library era O4 traced. Both files corrected with a per-deck
    table.
  - **Falsifier iii answered:** Release `-O3 -flto` against Debug `-O0 -g`,
    same source, two build directories with genuinely different engine
    hashes → **15/15 identical**. The usage warning stays as prudence,
    labelled as such.
  - **Clean-clone check, which is the real proof:** `git archive HEAD
    tests/parity | tar -x` into an empty directory runs 15/15.
  - Two of their own probes were vacuous on the first attempt and were redone
    — falsifier iii used a June 10 Debug binary (would have measured ten
    weeks of source change), and falsifier v's `sed` did not match, so the
    deck was never modified and "identical" meant nothing. **Both caught by
    the validator, not by me.**
- **(135) when the unit under test is a shared library, the executable's hash
  is not its identity.** A thin driver over a dylib is byte-identical across
  any change to the library, so hashing the driver measures nothing and a
  same-binary guard built on it stays silent precisely when it matters.
- **(136) `git ls-files` is a statement about the INDEX, not the
  repository.** My corpus finding cited `git ls-files tests/output` → 0. The
  finding is correct — `git ls-tree -r HEAD tests/output` at `b85b802d` is
  also 0 — but **the evidence I cited was not sound**, and I did not notice
  because it agreed with the answer. Checked at `d633c53e`, `git ls-files
  tests/parity` returns **zero for twenty files that commit had just added**.
  Use `git ls-tree -r HEAD` for "is this tracked", and `git archive HEAD |
  tar -x` for "would a clean clone have it" — which is what the validator
  did.
- **⛔ ACTION OWED (ops) — THE INDEX HAS DRIFTED AGAIN, and this time it is
  dangerous.** `git status --porcelain` at `d633c53e`: **34 paths staged as
  deletions (`D `), 56 `MM`** — and the 34 include **all twenty files
  `d633c53e` just committed**, the whole corpus. A `git commit -a` by anyone
  would delete the corpus in the commit after the one that added it. Same
  cause as the drift resolved during the H2 round: commits made through
  `GIT_INDEX_FILE` leave the main index stale. Fix is a plain `git reset`
  (refreshes the index, stages nothing). **Deliberately NOT run** — it would
  unstage whatever a concurrent session has staged on purpose. Run it when no
  other session is mid-stage, and re-check with `git ls-tree`, not
  `git ls-files`.
- **⚠ `tests/parity/snow/baseline/` is not tracked** (`d633c53e` tracked the
  deck, generator and README, deliberately not the generated baseline). So
  **the one mechanism in that directory that detects cross-round drift exists
  in a single working tree** — the exact condition the round was opened to
  fix, surviving one level down. `README.md` §3 now carries the warning
  rather than the claim. Either regenerate it from `gen_snow_parity.py` plus
  a recorded build, or drop the claim. Owed, not decided.

- **✅ RESOLVED 2026-08-22 — the git index, and it was worse than "drift".**
  Record: `tests/output/index_repair_2026-08-22/`.
  - **The cause was a stuck lock, not drift.** `.git/index.lock` was **zero
    bytes, dated 2026-08-21 05:58:13**, and `.git/index` carried the identical
    mtime — the main index had been frozen for **over a day** while HEAD
    advanced through seven commits made via `GIT_INDEX_FILE`. The 34 "staged
    deletions" and 56 `MM` were simply everything created or modified after
    05:58: present in HEAD, absent from a stale index.
  - **No work was ever at risk.** All 34 existed on disk, 33 byte-identical to
    HEAD; the one exception was this session's own `tests/parity/README.md`
    edit.
  - **The danger was real but unreachable while the lock stood.** Tested in a
    scratch repo rather than asserted: with that index state and no lock,
    `git commit -a` **does** commit the deletions (`git add -u` will not
    re-add a path the index has no entry for). With the lock present,
    `commit -a`, `add` and `reset` **all fail immediately**. The lock was both
    the cause and the guard.
  - **⚠ Removing the lock emptied the index.** `.git/index` dropped to
    **65 bytes** — `git write-tree` returned `4b825dc6…`, the empty tree — and
    `git status` went from 34 `D ` to **1850 `D `**, i.e. every file in HEAD.
    Whether the original index was already corrupt behind the lock or the
    removal replaced it is not determinable after the fact. **Worth knowing
    before anyone removes a stale lock again: the index may not survive it.**
  - **`git reset` repaired it, and the H2 protocol proved the cost was zero.**
    All 1850 HEAD-tracked paths checksummed before and after — aggregate
    `c08b8ef6…` both times, **not one byte changed**. `git ls-files` now
    returns 1850, matching `git ls-tree -r HEAD`; phantom deletions 0; staged
    entries 0. The 64 remaining ` M` are genuine in-flight work (the
    `DynamicWave`/`dwflow` and xsect/transect track, the GeoPackage fixtures)
    and were untouched — **the reset could not have unstaged anything
    intentional, because the empty index had nothing staged.**
- **(137) a lock file and the thing it protects can fail together, and the
  lock hides which one is broken.** For a full day the symptom was "the index
  is stale"; the diagnosis of record was "drift needing a reset". Both were
  true of what could be observed, and neither survived removing the lock —
  the index was empty underneath. **While a lock stands you are reading the
  state the lock froze, not the state of the thing.** Snapshot before the
  removal, not after: the checksum list that proved the repair cost nothing
  was taken while the lock was still on.

- **⬜ IN VALIDATION — water-age and heat decks for the corpus
  (`CORPUS_AGE_HEAT_DECKS_HANDOFF_2026-08-22.md`).** After ~15 rounds of
  building both, `tests/parity/README.md` §4 read **0 water-age decks, 0 heat
  decks** — the corpus could observe neither. Precedent and argument: the snow
  deck closed the same hole for `[SNOWPACKS]` and **found a real ledger defect
  on its first run**. Three decks from one generator on one shared network,
  corpus 15 → 18:
  - `age_legacy.inp` / `age_ard.inp` — **differ by one line.** Both move →
    shared age machinery; one moves → that engine's binding. `np = 0` on both
    is deliberate: a reserved-species-only deck is exactly what E5a found
    broken in all six `QualitySolver` loaders (`if (np <= 0) return` blocking
    external-inflow *volume*), fixed then and **unobserved since**.
  - `heat_parity.inp` — the only deck reaching the **`np + age + heat`**
    reported stride, where D-UT10's parallel-accumulator decision is
    load-bearing. Two flux families enabled, because with one there is nothing
    for D-H5e's merge to sum.
  - Network cascades `S1 → S2 → J1` with `S3` direct: run-on is where both age
    defects lived (A3, A4) and `S3` is the control that localises them.
  - **Taken ahead of H6 deliberately.** H6 would add heat physics to a corpus
    that cannot see heat — the configuration that produced the snow track.
  - **⛔ Nothing was run but the generator and the stub harness.** The decks
    have never been parsed by `openswmm`. Most likely failure is a parse error;
    the most likely *silent* failure is `age_ard` (np=0 under EULERIAN_ARD).
  - **The self-contained deck rule gained one exception:** heat has no inline
    form, so `heat_parity.heat` travels beside its deck. Component configs
    resolve relative to the `.inp`, not the cwd (`SWMMEngine.cpp` ~286) —
    **read, not run**; it is falsifier iii.
  - Owed and recorded: **no LID** (blocked on issue #131 — a deck written now
    bakes in pre-#131 behaviour and moves when the fix lands), no heat under
    ARD, `DRY_ELEMENT_TEMPERATURE` `AIR`/`DEFAULT` untouched, no snow+age
    deck, still 0 SI and 0 STEADY decks.
- **(138) a coverage count is not coverage, and a green one hides that
  best.** Every capability line in `tests/parity/README.md` §4 — snow, water
  age, heat, the reported stride — was a **zero** sitting behind a green
  "N/N unchanged" for months. The figure was true and stable the whole time,
  which is exactly why nobody read the composition behind it. Companion to
  (133): a claim repeated every round is the least likely to get checked, and
  a *count* repeated every round is the least likely to get decomposed.

- **✅ Age/heat corpus decks validated and COMMITTED `1da1d7ca`.** 18/18,
  generator `--check` clean and byte-reproducible, all five falsifiers
  behaving, **0.20 s for all three**. §5's leading guess was wrong — `age_ard`
  did **not** break; age varies and the control separates (S1/S2 48.94 h,
  S3 49.19 h). The companion-file claim held: deleting `heat_parity.heat`
  gives exit 5 and a loud error, and running the deck from its own directory
  is byte-identical to the runner's, so resolution really is relative to the
  `.inp`. **TSS is inert** — np=1 reaches the stride but the pollutant carries
  no mass, so the deck must not be read as exercising quality transport.
  **§1's argument from the snow-deck precedent held twice: the decks found two
  engine defects on their first run.**
- **⛔ FINDING 1 — the runoff ledger double-counts cascaded run-on. FIX IN
  VALIDATION (`RUNOFF_LEDGER_CASCADE_HANDOFF_2026-08-22.md`).** Same
  hydrology, two engines: precipitation 6.960 and infiltration 4.620 agree to
  the digit, **surface runoff 3.976 in against legacy's 2.348 in**, continuity
  **−23.667 % against −0.271 %** — the gap is exactly S1's own 1.628 in,
  booked once when S1 sheds it and again when S2 discharges it.
  `SWMMEngine.cpp:2357` added every subcatchment unconditionally; legacy
  guards it at `subcatch.c:761-765` (`outNode == -1 && outSubcatch != i`).
  **Not one of the fifteen previous corpus decks routes a subcatchment onto
  another subcatchment** — cascading is ordinary in real models and was absent
  from every deck we owned. Fix carries legacy's self-outlet exclusion, and is
  **ledger-only**: `runoff_runoff` has four readers, all reporting, so no
  `.out` may move and only cascade decks' `.rpt` continuity should.
- **⛔ FINDING 2 — the subcatchment `__TEMPERATURE__` column is never written.
  NOT FIXED, owed its own round.** Nodes and links carry live temperature
  (−4.147…17.66 °C); **every subcatchment reads exactly 0.0 all run.**
  `SWMMEngine.cpp:4645` fills the age row of `snapshot.subcatch_quality`;
  there is no temperature sibling, so it keeps its `assign(…, 0.0)` and the
  output plugin faithfully writes the zero. The value exists in
  `ctx_.heat_state.subcatch_runoff_temp`. Third instance of F8's family, and
  **the deck handoff's own column-presence check would have passed** — only
  reading the values caught it.
- **(139) a column that exists is not a column that is written.** The header
  is authored once at open; the values are authored every step, and **only one
  of those is evidence.** My §4.3 proposed checking the `.rpt` for a
  `__TEMPERATURE__` column "before believing a clean exit" — the column was
  there and the check would have passed while every value was zero. Sibling of
  (104) (a function unit-tested and never called) and of A2b's snapshot
  quality vectors, which had no writer at all.
- **(140) the deck that finds a defect is usually not the deck written to
  find it.** These three were written to observe age and heat. Finding 1 is a
  **runoff-ledger** defect with nothing to do with either — it surfaced
  because the network happened to cascade, and the network cascades because
  run-on is where the *age* defects lived. Coverage bought by aiming at one
  thing pays out somewhere else, which is an argument for adding decks that
  differ structurally rather than decks that differ in one option.

- **✅ FINDING 1 fix validated and COMMITTED `421e95c2`.** The ledger lands on
  legacy's number **exactly**: −23.667 % → **−0.271 %** on all three transport
  decks, legacy 5.x's figure to three decimals, and the other fifteen decks
  unchanged to the digit. **18/18 `.out` byte-identical** across two build
  directories with genuinely different engine libraries — §2's four-readers
  claim is verified, not argued. The gate fails at base with
  **direct=18157 cascade=22276**, the double-count as a number.
  **All three gaps §6 flagged are closed**, and one of them found a defect:
  - **`sheds_to_self` is live**, not decoration — drop it and a self-routed
    subcatchment's booked runoff falls 2.328 → 0.123 in. §6's "most likely
    thing to be wrong" was right to be flagged and turned out correct.
  - **iii and v clean.** `outlet_subcatch < 0` and `outlet_node >= 0` give
    byte-identical totals on five decks — genuine complements, so §5's
    "bigger finding" does not fire. A 3-deep cascade books 0.318 against an
    all-direct 0.625: only the last counts, depth is irrelevant to a
    per-subcatchment guard, and legacy agrees to the digit on both.
  - **ii failed the gate on the SETUP leg** (direct > 0, actual 0) rather than
    where §5 predicted: inverting stops *both* fixtures counting, so there is
    no total left to compare. The gate still failed — at the leg that exists
    to catch a fixture producing nothing. **A correct prediction of "the gate
    fails" can still be wrong about which assertion does the work.**
  - **My ctest arithmetic was wrong.** §4.2 said 160/161; the gate joined the
    existing `test_engine_massbalance` binary, so the ctest total stays 160.
    **ctest counts registered tests, and this suite registers one per binary**
    — which is also why "216 test gates" and "160 ctest tests" are different
    units and neither converts to the other.
- **⛔ FINDING 3 — self-routed subcatchments RECIRCULATE their own runoff.
  NOT FIXED; the largest of the three.** Found while writing falsifier i's
  fixture. `selfroute` books **2.328 in against legacy's 0.417** — 5.6×, with
  **−265 % continuity** — while direct, 2-deep, 3-deep and all-direct all
  agree with legacy to the digit. **The ledger is not the problem; the
  divergence is one layer up.** Legacy carries `!= subcatchIndex` in **three**
  places: **run-on distribution (`subcatch.c:546-548`)**, the ledger (`763`),
  and **washoff (`surfqual.c:363`)**. `421e95c2` gives us only the second, so
  a self-routed subcatchment feeds its own runoff back to itself and the
  ledger faithfully reports a hydrology that is already wrong. Changes routed
  water → own round, own falsifiers. **No corpus deck has a self-route**; the
  fixture is written.
- **⬜ FINDING 2 fix IN VALIDATION
  (`SUBCATCH_TEMPERATURE_WRITER_HANDOFF_2026-08-22.md`).** One writer added
  beside the node (`:4588`) and link (`:4645`) ones, at `:4682`. It sits
  **outside** the `has_runoff` gate the age column obeys — the only judgement
  call in the changeset: age is gated by legacy's washoff convention, but
  0 °C is a real temperature and **D-H5c exists so the dry value is the
  deck's choice**, so blanking it would silently ignore
  `DRY_ELEMENT_TEMPERATURE`. Two gates: one reads **values out of the finished
  `.out`** (a state gate cannot fail on this — lesson 104) and asserts the
  column is neither identically zero **nor constant**; the other stops the
  rain and asserts a dry subcatchment still reports, which is the only thing
  that can catch the write being moved back inside the gate.
  **`heat_parity.out` is expected to MOVE** — first round where that is the
  pass condition.
- **(141) the fixture written to close a verification gap found a defect the
  gap was not about.** Falsifier i existed to answer "is `sheds_to_self` live";
  writing its fixture exposed Finding 3, which is a routing defect in a
  different file. Companion to (140) — that was about *coverage* paying out
  elsewhere; this is about *falsifiers* doing it. Closing an honest "I could
  not check this" is worth more than the check itself suggests.
- **(142) a guard that appears once in our code may appear three times in
  legacy, and porting one site is not porting the invariant.** We matched
  `subcatch.c:763` and treated the divergence as closed; two sibling sites
  carrying the same `!= subcatchIndex` condition were never looked for. When a
  fix is justified by "legacy guards this here", **grep legacy for the
  condition, not the line.**

- **✅ FINDING 2 fix validated and COMMITTED `29cbc361`. The writer was right;
  BOTH gates were wrong.** 159/160 ×3; **17/18 corpus decks identical and
  `heat_parity` moved** — §4.3's pass condition, in the right direction. The
  movement is arithmetically exactly one float column: **2003 of 51520 bytes**
  against 3 subcatchments × 4 bytes × ~167 periods = 2004. Values S1
  −4.147…19.59, S2 −4.147…17.67, S3 −4.147…19.68 °C against nodes/links at
  −4.147…17.66 — **shared minimum, subcatchment maxima slightly above the
  piped water**, which is what a surface under warm air should do.
  - **⛔ Gate defect 1 — I misread the reader API in both gates.**
    `swmm_output_get_subcatch_attribute(handle, subcatch_idx, period, …)`
    returns **all variables for ONE subcatchment**; the second parameter is an
    **object index, not an attribute code**. I passed the attribute code,
    indexed past a two-element list, and got `-1` every period. The correct
    call is `swmm_output_get_subcatch_result(handle, period, var, values)` —
    **one variable for ALL objects.** The two names are inverted from what
    they suggest, which is worth its own docs/API note.
  - **⛔ Gate defect 2 — gate 2 PASSED under falsifier i**, the one defect it
    was written to catch. Its SETUP leg first failed honestly (zero dry
    periods at 60 min — the subcatchments are still shedding 0.0624 cfs at the
    last report); extending to 360 min gave 44 of 72 "dry" periods, the gate
    passed, **and it passed with the write moved back inside `has_runoff`.**
    The two "dry"s are **different predicates**: the writer tests
    `runoff[s] != 0.0`, an exact double on a decaying quantity that
    essentially never turns false, while the `.out` column rounds to `0.0f`
    long before. All 44 periods were **dry to the reader and wet to the
    writer**. Fixed with `starve_receiver` — a subcatchment on a gage that
    never rains, exactly 0.0 from step 1. Falsifier i now reports
    `12 of 12 dry subcatchment-periods report 0.0 degC`, gate 2 fails and
    gate 1 passes: the asymmetry §3 asked for.
  - **Falsifier iii: `temp_col` is provably redundant.**
    `subcatch_runoff_temp` is empty when heat is off — `resizeWatershed` is
    only reached past `!heat_transport` (`HeatWatershed.cpp:100,112`) — so the
    size check already guards, at this site and at its node/link siblings.
    Kept as belt-and-braces, recorded as such rather than left implied.
  - **Falsifier v answered a different question and SHARPENED §2.** At
    reported-dry periods on `heat_parity`, temperature is nonzero — **and so
    is age**, from the same exact-comparison root cause. Not a divergence:
    legacy uses the identical test (`subcatch.c:929`). **The washoff
    convention is narrower than it reads** — it blanks only a subcatchment
    receiving nothing at all.
- **(143, theirs) "dry" is not one predicate.** Make the fixture dry **by
  construction, not by waiting.** A decaying quantity tested with `!= 0.0`
  never turns false; the same quantity rounded into a `float` output column
  turns false early. A gate that gets its "dry" from the reader and its
  behaviour from the writer is testing two different states and can pass on
  the defect it exists for.
- **(144) copying a call's SHAPE is not reading its contract.** I wrote both
  gates against `swmm_output_get_subcatch_attribute` by mirroring
  `test_output_quality.cpp`'s `get_node_attribute(h, 0, period, …)` — where
  the `0` is an **object index** — and read it as an attribute slot because
  the parameter I wanted to vary sat in that position. Never ran it. Same
  family as (126) and (130): **a read is not a run**, now at the gate level,
  and doubled because I reused the mistaken shape in the second gate.
- **↩ CORRECTION owed back to the 2D session — the flag was wrong, do not
  relay it.** The round reported that the second `refreshRenderFieldsIfStale()`
  call "is not inside `#ifdef OPENSWMM_HAS_2D` while the first one is".
  Checked by walking the preprocessor stack over the whole file: **line 4432
  guards `['#ifdef OPENSWMM_HAS_2D']` and line 4732 guards
  `['#ifdef OPENSWMM_HAS_2D']`.** The `#ifdef` opens at 4722, the `else if` at
  4723, the call sits at 4732, and the `#endif` is at 4736 — **both calls are
  guarded.** Nothing to report to that session.

- **⬜ FINDING 3 fix IN VALIDATION
  (`SELF_ROUTE_RECIRCULATION_HANDOFF_2026-08-22.md`).** `assembleRunon`
  (`:6503`) gains `&& out_sc != i` — legacy's `subcatch.c:546-548` — and the
  washoff ledger (`:2876`) gains `outlet_node >= 0 || outlet_subcatch == i`,
  legacy's `surfqual.c:363`. **The run-on guard closes three seams at once:**
  `addRunonAge` and `addRunonTemperature` live in the same branch, so a
  self-routed subcatchment was feeding its own **age** and its own **heat**
  back to itself as well as its water. **First of the three findings that
  changes routed water**; no corpus deck self-routes, so the expectation is
  still 18/18. The washoff site **splits one statement in two on purpose**:
  `qual_runoff_load` becomes conditional, `subcatches.total_load` stays
  unconditional — legacy's own arrangement (`surfqual.c:356` sits above its
  guard), because the per-subcatchment total is *what this subcatchment washed
  off* and the ledger term is *what the system received*. The gate asserts
  **equality** (`self == direct`), not inequality: a self-route is a **no-op**
  in legacy's model, so equality is the real statement and is far stronger
  than "smaller than the broken value".
  **Weakest part, flagged:** the washoff guard has **no gate** — the new one
  is volumetric and a quality fixture is owed; and age/heat on a self-route
  are fixed by construction and unasserted, which is the reasoning lesson 104
  keeps punishing.
- **A question neither engine answers: is a self-route legal input?** Legacy
  silently treats `SA → SA` as a no-op and we now match that. **Neither
  warns**, so a user who writes it by typo gets no signal at all. Worth a
  warning; deliberately not scoped into the fix round.
- **(145) a deck added alongside its own fix cannot demonstrate that it would
  have caught the defect.** No corpus deck self-routes, and adding one *in
  this changeset* was tempting and declined: the deck would be born passing.
  Add it in a later round, against a build that still has the defect, or the
  addition proves only that the fix is self-consistent. Corollary of the snow
  and age/heat rounds, both of which earned their findings precisely because
  the deck predated the fix.

- **✅ FINDING 3 fix validated and COMMITTED `69467241` — correct as
  delivered, both hunks, both legacy sites.** Gate fails at base with
  `self=101420.31 direct=18157.17`, ratio **5.5857** against §1's 5.6×. Five
  decks, both engines, after: `selfroute` joins the other four at legacy's
  **0.417 acre-feet**; in inches 2.794 → 0.500 and continuity
  **−265.245 % → −1.160 %**, both legacy's, with the whole Runoff Quantity
  block diffing clean on all five. **Units correction: §1's table was
  acre-feet and my source comment said "in"** — ratio right, unit wrong; fixed
  at source.
  - **Falsifier iv is WORSE than predicted:** a self-route as the *only*
    subcatchment is **7.5× / −529.722 %**, not 5.6× — nothing dilutes it.
  - **Falsifier v measured rather than assumed:** SA's `__WATER_AGE__` moves
    2.207471 → **4.199100 h, exactly the direct deck's value**, control
    byte-identical. **Temperature stays flat 20.0, so the heat seam is still
    unobserved** — §5's gap survives the round that was supposed to close it.
  - **§5's weakest point is closed:**
    `WashoffLoadIsBookedOnlyWhenItReachesTheSystem`. Each falsifier is caught
    by a **different assertion** — i by the self-route leg (1.873 ≠ 1.369),
    ii by the cascade leg (1.522 > 1.369) **while both volumetric gates
    pass**, iii by the per-subcatchment totals (SA total 0 vs 0.746). ii is
    the point of the whole gate. Note **ii and iii need the cascade deck**:
    on a self-route `outlet_subcatch == i` is true, so that guard is inert
    there.
- **⛔ "4 decks moved" WAS MY HARNESS, NOT THE FIX — and it lied in the most
  plausible possible way.** The run reported `force_legacy`, `force_ard`,
  `orif_legacy`, `sdm_struct_dw_ard` moving: **precisely the quality decks,
  exactly what a washoff-guard defect would look like.** The two build
  directories had different CMake options — `OPENSWMM_FAST_MANNING_POW` /
  `OPENSWMM_FAST_XSECT_LOOKUP` OFF in one, ON in the other — so the run
  measured `24d51e6e`'s xsect accelerator. Matched pair: **18/18 identical.**
  `run_corpus.sh` could not catch it: **my vacuity guard points the wrong
  way**, warning when the two engines are the SAME file, when the dangerous
  direction is two secretly DIFFERENT builds. **Fixed this round** — the
  runner now diffs `OPENSWMM_*`, `CMAKE_BUILD_TYPE` and `CMAKE_CXX_FLAGS`
  between the two `CMakeCache.txt` files and says so **before** the table.
- **🛑 FINDING 4 — the node injection double-counts run-on. NOT FIXED, and it
  is bigger than the three before it.** `SWMMEngine.cpp:2183` feeds the node
  `q_runoff + q_runon`; legacy's `subcatch_getWtdOutflow` returns **runoff
  alone**, because run-on is *already inside* the receiver's `newRunoff`.

  | deck | legacy | ours (base AND patched) |
  |---|---|---|
  | cascade | 0.218 | **0.511** |
  | three_deep | 0.318 | **0.536** |

  The excess on cascade is **0.293 acre-feet against the donor's own runoff of
  0.294**. **Our conveyance receives 2.3× what our own runoff ledger says left
  the surface, and neither continuity check notices.** Finding 1's shape one
  layer over, and pre-existing — unchanged by `69467241`. **Not a one-line
  deletion:** `runon_inflow` also carries LID-drain and outfall run-on.
- **⛔ FINDING 5 — the Subcatchment Washoff Summary prints 0.000 on every
  ordinary deck.** It divides by **453592** while the ledger row prints the
  same mass variable raw; the ratio was measured at **exactly 453592.0**. One
  of the two is in the wrong unit.
- **⛔ FINDING 6 — buildup/washoff already diverge from legacy on the simplest
  deck**: legacy **0.885 / 0.000** against our **2.500 / 1.369**. This blocked
  the cross-engine comparison §4.5.ii asked for — a legacy number would have
  measured *this* gap, not the guard under test. **Predates all four findings
  above and is unscoped.**
- **(146, theirs) a moved deck is the loudest signal the corpus produces, and
  it can be wrong in exactly the shape you are looking for.** Four quality
  decks moved during a washoff-guard round. Before reading a movement as a
  finding, **diff the two build configurations** — the instrument compares
  two binaries and says nothing about how they were configured.
- **(147) two self-consistent balances can both be wrong, and each one
  certifies the other.** Finding 4: our runoff continuity closes, our routing
  continuity closes, and the conveyance receives **2.3×** what the surface
  ledger says left. **A continuity check verifies internal consistency, not
  correspondence across the seam between two subsystems.** Every ledger fix in
  this program so far (F8, Finding 1, Finding 3) was found by comparing
  against *legacy*, never by a balance failing — because a balance that
  double-counts on both sides of its own seam still closes.

- **✅ Config-guard round validated and COMMITTED `84984990`. The failure I
  predicted did not happen; the opposite one did.** §4.1 was fine — the cache
  resolved from **every real layout** (`bin/Release/`, three `src/cli/`
  directories, and `src/legacy/cli/`), 28 lines each, no "not compared" NOTE
  anywhere. §4.2 reproduced the false positive exactly, same four decks, same
  byte counts, banner first.
  - **⛔ §4.3: the guard cried wolf on the pair that produced the trustworthy
    18/18.** `build/darwin` vs `build/darwin-tests-release` fired on
    `OPENSWMM_BUILD_TESTS` and both `*PRERELEASE` — **version strings**, none
    of which can change a number. **§5 worried the filter was too NARROW; it
    was too WIDE.** Prerelease strings drift between any two build
    directories configured at different times, so the guard would have fired
    on essentially every honest before/after — **and a banner that fires every
    time is one nobody reads, which is the failure it exists to prevent one
    level up.** Repaired with a `CFG_IGNORE` deny-list: six
    `OPENSWMM_BUILD_*` target switches, both `*PRERELEASE`, two install-side
    entries. **A named list, not a `BUILD_*` pattern** — `OPENSWMM_BUILD_2D`
    and `BUILD_GPU_PLUGIN` match that prefix and *do* change what the engine
    computes.
  - Sweep clean, including two new cases they added (`OPENSWMM_PRERELEASE`
    silent, `BUILD_2D` flipped fires). iv confirms warn-not-refuse: made
    fatal, the Release-vs-Debug run dies at the banner.
  - **The three-parent search has no margin left** — the legacy CLI already
    needs all three. `CMAKE_CXX_COMPILER` still uncompared.
  - **A trap worth knowing:** piping `run_corpus.sh` into `head` SIGPIPEs the
    script mid-run and **looks exactly like the guard aborting it**.
- **(148, theirs) a warning that fires every time is one nobody reads.** The
  filter's job is not to catch everything that differs; it is to catch what
  can change a NUMBER. Include a version string and the guard becomes noise,
  which is the same failure it was built to prevent, one level up. And when
  writing the deny-list, **name the entries** — `OPENSWMM_BUILD_2D` and
  `BUILD_GPU_PLUGIN` match a `BUILD_*` pattern and are load-bearing.
- **⬜ FINDING 4 fix IN VALIDATION
  (`NODE_INJECTION_RUNON_HANDOFF_2026-08-22.md`).** `SWMMEngine.cpp:2183`
  drops `+ q_runon`; legacy's `subcatch_getWtdOutflow` is
  `(1-f)·oldRunoff + f·newRunoff` and nothing else.
  - **The per-contributor question I called "most of the round" resolves
    UNIFORMLY, and structurally.** `assembleRunon` sums every contributor —
    subcatchment cascade, LID drain (Gap #25), outfall return (Gap #28) —
    into **one** `runon_inflow[]`, and `Runoff.cpp:331-333` consumes that
    array **wholesale**: `precip += runon_q / total_area`. **The solver cannot
    distinguish contributors; it reads one lump.** So there is no contributor
    for which the node addition is legitimate.
  - **The edge I checked for and did not find:** `Runoff.cpp:322` sets
    `total_area = soa_.area[ui]`, the **full** subcatchment area — the same
    quantity the node loop already guards on — so the two guards coincide and
    no subcatchment receives run-on without consuming it.
  - **The gate asserts CORRESPONDENCE ACROSS THE SEAM**, not another balance:
    `SWMM_ROUTING_WET_WEATHER == SWMM_RUNOFF_RUNOFF` on a deck where nothing
    can be lost between surface and node. Both existing balances closed while
    the defect was live, which is exactly why a third would have been
    useless. **The no-cascade fixture runs as a CONTROL** — it must pass at
    base and after, because the ordinary case is every model anyone has.
  - **First fix in this sequence that moves `.out` files broadly.** The three
    transport decks should move; if an `sdm_*` deck moves, check its
    `[SUBCATCHMENTS]` outlets before calling it a regression.
  - **⚠ `old_runon_inflow` is now unread and I did NOT remove it.** It carries
    rotate/reset/**serialisation** machinery in `SubcatchData.hpp` (line 821
    looks like a hotstart field enumerator), so removing it could change the
    hotstart layout. CLAUDE.md §3 says clean up what my change orphaned — but
    not at the cost of a format change nobody asked for. **Flagged for a
    decision, not taken.**
  - **Owed:** LID-drain and outfall run-on are **reasoned about, not
    measured** — no deck exercises either, and §2's argument stands or falls
    on falsifiers iii and iv.
- **⚠ A comment/implementation mismatch found on the way, not fixed:**
  `Runoff.cpp:327-329` says run-on is distributed "over the **non-LID** area"
  while `total_area` is the full area. One of the two is wrong. It does not
  change Finding 4's fix — the guards coincide either way — but it is a live
  discrepancy in a load-bearing comment.

- **✅ FINDING 4 fix validated and COMMITTED `55a70839` — the engine hunk was
  right as delivered.** Corpus **15/18**, exactly the three transport decks,
  and **the config guard was silent on its first real round**, so the movement
  is attributable. They checked every deck's outlets rather than assuming: the
  movers are `S1→S2`; the SDM decks route `S1→J1 … S7→J10`, every one to a
  junction, **so identical is right for the right reason.** All five fixtures
  now match legacy: cascade 0.511→**0.218**, three_deep 0.536→**0.318**,
  direct/selfroute/three_flat unmoved.
  - **⛔ MY GATE'S TOLERANCE WAS BELOW THE ACHIEVABLE FLOOR.** It failed at
    base *and* with the fix — control 18156.2399 vs 18157.1738. The two totals
    **integrate on different clocks**: the ledger on the runoff step, the
    routing total on the interpolated routing step. They **measured** the floor
    by sweeping the wet step rather than assuming — 15 min 5.1e-5, 5 min
    2.0e-5, 1 min 6.6e-6, 20 s 2.2e-6, **monotone to zero: quadrature, not a
    leak.** Repaired to 1e-3 (20× the floor, three orders below the defect's
    1.35) **plus a statement that does not rot with a chosen number**: cascade
    seam error ≤ 10× the control's — 1.5× with the fix, **26175.9×** without.
  - **🛑 Falsifiers iii and iv — the two contributors I could only reason
    about — are now MEASURED, and both were real:** `lid_drain` 1.9412 →
    1.0000 (legacy 1.0000), `outfall_return` 2.0759 → 0.9986 (legacy 0.9987),
    control 1.0000 throughout. §2's structural argument holds for all three
    contributors **by measurement now**.
  - **The outfall deck was MANUFACTURING water, not merely mis-booking it.**
    Its surface runoff collapses **10.020 → 0.705 acre-feet** against legacy's
    0.779, because the doubled node inflow fed the outfall that fed it back —
    a feedback loop, so the error compounded rather than doubling.
  - They also fixed `SWMMEngine.hpp:464`'s doc comment, which still said
    "runoff+runon". **Orphaned by my change and I missed it.**
- **(149) a tolerance is a claim about the noise floor, and it has to be
  MEASURED, not inherited by analogy.** I used 1e-6 because the neighbouring
  gates do. Those compare quantities integrated on the *same* clock; mine
  crossed a seam between two integrators with different steps, where the floor
  is ~5e-5 and falls with the step. **The durable form is a ratio, not a
  constant** — "cascade error ≤ 10× the control's" survives a timestep change,
  a machine change and a refactor, and reads 26175.9× when the defect is back.
- **(150) a double-count inside a feedback loop amplifies rather than
  doubles.** Outfall return: the doubled node inflow fed the outfall, which
  fed the subcatchment, which shed more. 10.020 against 0.779 is **13×**, not
  2×. **When a mis-booking sits on a cycle, the magnitude of the error says
  nothing about the size of the mistake.**
- **(151) I swept for orphaned CODE and not orphaned PROSE.** My change made
  `SWMMEngine.hpp:464`'s doc comment false — it still described `runoff+runon`
  — and CLAUDE.md §3's "clean up your own mess" covers comments as surely as
  variables. **A stale comment left by a fix is worse than one that was always
  wrong: it now carries the authority of having been updated nearby.**
- **⚠ The `b85b802d` fixture guard has a model gap, and it is mine.**
  `test_engine_concurrent` aborted **once in twelve** full runs (0/25
  standalone, 5/5 solo): `test_concurrent_engines.cpp:180` **reads**
  `site_drainage_model.inp` from the shared `data/` cwd while other tests
  **write** `site_drainage_model.out` there. My guard catches **duplicate
  fixture names**; it has no model for a **read/write race on a shared
  input**. Not mine to chase, but the guard's limits are.
- **⛔ FINDING 7 — the LID deck sheds 34× legacy's water.** `lid_drain` gives
  **15.482 acre-feet against legacy's 0.456**, unchanged by Finding 4's fix.
  The LID area/unit family (`df7bdf12`, issue #131) surfacing on **the first
  deck ever built to exercise `DrainTo`**. **No corpus deck has an LID at
  all** — which is exactly why `tests/parity/transport/README.md` §5 defers the
  LID corpus deck until #131, and this is the measurement that justifies the
  deferral.
- **⛔ FINDINGS 5 and 6 ARE ENTANGLED, and tracing them turned up an eighth.**
  Investigated this round rather than fixed, because fixing one blind would
  mask the other:
  - **Our washoff mass is in mg; legacy's is in lbs.** `total_washoff_load` is
    mg/sec (`SWMMEngine.cpp` ~2871: `w_lid_rain = c_rain37 * L_PER_FT3_37 *
    v_lid_rain;  // mg`), so `mass = total_washoff_load * dt` is **mg**.
    Legacy applies `Pollut[p].mcf` **at source** — `massLoad = cOut * vOut2 *
    Pollut[p].mcf` — so **both** `Subcatch.totalLoad` (`surfqual.c:357`) and
    `RUNOFF_LOAD` (`:366`) are already in **lbs/kg**.
  - **So Finding 5's answer is the opposite of the obvious one: the SUMMARY is
    right and the LEDGER ROW is wrong.** The summary applies `MG_TO_LBS`; the
    ledger row prints the same variable raw under a header claiming mass
    units. The exactly-453592 ratio is that, and nothing else.
  - **Finding 6 may partly dissolve, and partly not.** Our washoff 1.369 mg is
    **3.0e-6 lbs**, and legacy's is 0.000 lbs — those plausibly *agree*. But
    buildup legacy **0.885 lbs** against our 2.500 does **not** agree under
    either reading. **That points at the ledger MIXING UNITS ACROSS TERMS**,
    which no single conversion can fix and which must be audited per term.
  - **🛑 FINDING 8 — `qual_bmp_removal` has ZERO write sites.** Declared,
    resized, moved, and enumerated in `SimulationContext.hpp`; **written
    nowhere in `src/engine/`.** Legacy writes it at `surfqual.c:352`
    (`massbal_updateLoadingTotals(BMP_REMOVAL_LOAD, …)`). **Fourth instance of
    F8's family** — a report row rendered from a variable nothing fills — after
    the snapshot quality vectors, the snow ledger rows, and the subcatchment
    temperature column.

- **✅ Their units audit round (validated `55a70839`'s scoping, committed
  their own probes) REFUTED my §1 and set the design.** My trace said our
  washoff was mg and the summary correct; **measured on a known-mass deck
  (EMC 100 mg/L, V = 18157.174 ft³): the booked mass is exactly C·V in
  mg/L·ft³, and BOTH printed numbers are wrong** — ledger 1,815,717.383
  (16057× legacy's 113.082), summary 4.003 (28.25× under). I generalised from
  `w_lid_rain`, the ONE path that carries the conversion; the EMC path
  carries neither. The strongest evidence was three lines apart the whole
  time: the Link Pollutant Load comment says "ft³ × mg/L" and converts fully;
  the Washoff Summary says "mg" and half-converts. Their audit table: **three
  of nine terms right** (the buildup family, user mass), three mg, one
  five-contributor mix, one unwritten, one dead. Plus: **Finding 6 is not
  units at all** — legacy's 0.000 was CORRECT (buildup accrues after storms;
  the deck had no dry period), **Finding 9** (dead `+=` discarded every run),
  **Finding 10** (continuity error vacuous — 1.8M units left an empty system
  under a printed 0.000), **Finding 11** (vendored legacy `landuse.c:633`
  `>= 0.0` since `03ed283a` where stock EPA has `== 0.0` — the parity
  REFERENCE was corrupted; measured inert on the EMC deck, patched-and-
  relinked to prove it), and **five hardcoded 43560.0** (the F9 shape, two in
  the quality path).
- **(152) the path you sampled is not the chain you audited.** My §1
  generalised one expression (`w_lid_rain`, correctly commented `// mg`) to
  the whole accumulator, and the accumulator has five contributors in three
  unit systems. The exactly-453592 ratio "corroborated" the wrong hypothesis
  because BOTH hypotheses predict it — it measures the two printers'
  disagreement, not either one's correctness. **A ratio between two outputs
  of the same variable cannot tell you the variable's units.** Only the
  known-mass deck could, which is why §5's "confirm by measurement" was the
  only part of my scoping that survived contact.
- **⬜ UNITS FIX IN VALIDATION
  (`QUALITY_LEDGER_UNITS_FIX_HANDOFF_2026-08-23.md`).** Fixes 5, 8, 9, 10, 11
  and the formulation gaps in one parity round: one internal convention
  (concentration mass/s), `mcf_p` (legacy `landuse.c:167-169`) applied once
  at every ledger booking, summary prints raw, EMC gains `LperFT3` and drops
  its wrong `UCF(FLOW)`, EXPON gains `/3600` and `/mcf`, the buildup cap
  compares like with like, `conc = load/q/LperFT3` = mg/L
  (`surfqual.c:370`), `qual_bmp_removal` written (`surfqual.c:352`), the dead
  `+=` removed, the error gets legacy's third branch (`massbal.c:908-911`),
  vendored `landuse.c:633` restored to stock. **The gate is the audit's
  acceptance test**: both printed numbers from the finished `.rpt` on the
  known-mass deck, asserting they agree AND carry `C·V·LperFT3·UCF(MASS)` —
  agreement alone would pass two numbers wrong together. One gate defect
  caught in draft: `"Surface Runoff"` appears in both continuity blocks and a
  bare `find()` lands on acre-feet; anchored on `"Runoff Quality Continuity"`.
  **Owed out of the round:** EXPON/RATING known-mass gates (need the buildup
  deck with a dry period, which is also §5.5's first meaningful cross-engine
  buildup comparison — both sides changed, so it is a fresh measurement),
  µg/counts branches read-not-measured, the 43560s (O6), the ponded residual
  parity gap.

- **✅ TRACKER RECONCILIATION (2026-08-25) — no engine logic changed.** Three
  parallel audits **against the code, not the documents**
  (`PROGRAM_REVIEW_2026-08-25.md`). What the reconciliation changed here:
  - **Phase 1x added** — the X/Y/Z/closeout tracks now appear in the canonical
    tables. Until today `grep X4|Y0|Z1` in this file returned **zero hits**
    for ~20 validated commits.
  - **IO3 and IO5 settled** — this file contradicted *itself* on both. Both
    are **PARTIAL**, and the code says so in its own words: `InpWriter.cpp:2574`
    admits there is no per-component `saveData()`, then `:2580-2586` **warns
    the user that embedded `[REACTION_*]` sections are lost from the save**.
  - **R5 was stale, not open** — the C half shipped under labels E-C1/E-C3,
    ~38 entry points, 804 LOC. Only Python and MCP are missing.
  - **Phase 5 and the GUI track re-marked** — LARD is mostly delivered; three
    GUI editors are implemented, one of them 1150 lines.
  - **Phase 3 relabelled `2D-S1…2D-S7`** to end the five-label collision with
    the snow track. **The unstarted side was renamed, not the finished one** —
    the snow labels are baked into sixteen handoff documents that are
    historical record, and renaming them would rewrite the record to fix a
    forward-looking problem.
  - **Phase 2 and Phase 4 gained verification notes** — HC1/HC2 are *refused*,
    not unstarted; G1/G2 have two traps that make them read as landed.
  - **Two false in-source docblocks repaired** — `ArdEngine.hpp` listed
    treatment/sources/ledger as "pending (E5)" five rounds after E5a and E5b
    landed, and `SimulationOptions.hpp` called LARD "skeleton dispatch only"
    five rounds after X2. **Both were read as authoritative by the audit.**
- **(153) three trackers that disagree with the code will also disagree with
  each other, and the disagreement is not random — it is a function of who
  wrote each one.** `PROGRESS.md` was blind to every track outside one
  session's line of sight; this roadmap was blind to the X/Y/Z rounds because
  they were defined in a subplan; the handoffs were right about their own
  round and wrong about the program ("THE ENGINE TRACK IS COMPLETE", X5 §10,
  with two engine commits still to come). **Each document was accurate about
  what its author could see.** The fix is not more diligence per document; it
  is one canonical table that every round is required to touch.
- **(154) a stale comment outranks a stale document, because it sits next to
  the code and inherits its authority.** Two docblocks — `ArdEngine.hpp:45-47`
  and `SimulationOptions.hpp:88-91` — each described work that had landed five
  rounds earlier, and a code-first audit trusted them *over* the plan docs on
  the reasonable theory that source comments are closer to the truth. They
  were the least accurate artefacts in the program. Sibling of (151): a fix
  must update the prose its own change falsifies, and a step that lands must
  update the docblock that says it hasn't.
- **(155) the phrase "not started" hides two very different states.** Phase 3
  is not started: there is no code and nothing prevents it. Phase 2 is not
  started AND the code **actively rejects** the thing it would deliver — a
  library-backed `[PROCESS_COMPONENTS]` row hard-errors with "arrives with
  plan phase HC2". Same tracker glyph, very different costs. Worse, the
  in-process registry uses HydroCouple-flavoured naming
  (`org.hydrocouple.openswmm.*`), so **the phase reads as partly done to
  anyone grepping for the vocabulary** — including our own documents.

- **✅ G0 SIGNED OFF 2026-08-25 (user), ten days after the plan landed.** All
  five new decisions and the carried draft block closed; see the Phase 4 row
  and `G0_SIGNOFF_RECOMMENDATION_2026-08-25.md`. **Two findings came out of
  reviewing rather than rubber-stamping:**
  - **D-N1's risk entry was stale.** §11 called the runtime tier count
    "surgical but shared-code" because it "touches the surface solver's fixed
    arrays". Measured: `cells_by_tier_` and `edges_by_tier_` are **already
    `std::vector<std::vector<int>>`**. The only fixed things are a telemetry
    `std::array<long,8>`, two clamps and a parser bound. **Approved and
    re-rated from risk to chore.**
  - **⚠ The protection §11 relies on does not exist.** "Gate 10's bitwise
    surface regression" — `tests/parity/MANIFEST` has **19 decks and none is
    2D**. `test_2d_lts_equivalence.cpp` covers the tier count but is a
    conservation/equivalence gate, **not bitwise**. Sign-off made a 2D corpus
    deck a **precondition** of implementing D-N1.
  - **D-N2 was DEFERRED by decision rather than guessed** — no `C_GW`/`C_COL`
    value is defensible before the closures run, so G1 ships conservative and
    logs the achieved Δt, and step 18 sets the default from measurement
    alongside the αL thresholds already deferred there.
- **(156) a risk register ages faster than the code it describes, and in the
  direction that costs you work you did not need to do.** D-N1 sat unsigned
  for ten days partly because its own plan described it as touching shared
  fixed arrays; the arrays had been written dynamically before the sentence
  was published. **A decision deferred on a stale risk is a decision deferred
  for no reason** — and the same review found the opposite error in the same
  section, a protection claimed that was never built. Sibling of (154): the
  document nearest the code is not automatically the one that tracks it.

- **🛑 FINDING (2026-08-26) — SAVING A MODEL SILENTLY DESTROYS EMBEDDED
  REACTION DATA, INCLUDING FROM THE GUI. User-facing data loss; owed its own
  round.** Reproduce: open a deck with embedded `[REACTION_*]`, change
  anything, save. **The reaction system is gone and nothing says so.**
  - The warning exists (`InpWriter.cpp:2580-2586`) and is well written. It is
    gated on an **optional `warnings` sink defaulted to null**, and **all
    three production callers pass nothing** — `openswmm_model_impl.cpp:265`
    and `:276` (the C API, which is what the GUI's
    `swmm_model_write_with_plugin` goes through) and
    `DefaultInputPlugin.cpp:203`.
  - **The test that certifies the behaviour calls the writer directly WITH a
    sink**, so it structurally cannot catch this. Lesson 91's family, one
    level out: the gate exercises a code path production never takes.
  - The GUI has no handling for the message even if it were emitted.
  - **Scope for the fix round:** route the warning to `ctx.warnings` (or the
    plugin's diagnostic channel) so every caller gets it by default; decide
    whether a save that loses model data should **warn or refuse**; add a
    gate that drives the **production** path, not the writer directly. IO3
    proper (per-component `saveData()`) is the real cure and is larger.
- **(157) a wrapped `grep`/`find` that silently skips ignored paths turns
  every count into a lower bound of unknown depth.** The shells here hide
  gitignored files, and this repo ignores **all of `plans/`** — which is how a
  true count of 38 was reported as **1**. It fails *silently* and in the
  direction that looks like a clean result. **Any count that matters must
  state its tool and be re-taken with one that does not filter** (`git
  ls-files --others --ignored`, `rg --no-ignore`, or plain `ls -R`).
- **(158) `plans/` is NOT IN GIT — `/plans/transport/*` is gitignored
  (`.gitignore:11`), and `git ls-tree -r HEAD plans/` returns ZERO.** Every
  document this program has produced — this roadmap, `PROGRESS.md`, the
  divergence register, ~40 handoffs, the program review, the G0 sign-off —
  exists **only in one working tree**. That is precisely the defect the corpus
  round fixed for the decks (`d633c53e`), one level up, and **the round that
  found it did not think to check itself.** It also means the prepared commit
  message in the reconciliation handoff describes files git will refuse: the
  commit would land two docblock edits under a message about reconciling
  trackers. **Decision owed:** the ignore was a deliberate 2026-08-21 choice,
  so either it is reaffirmed (and the documents are accepted as
  single-machine artefacts, which the G0 sign-off's auditability problem
  should inform) or it is reversed for the plan corpus.
- **(159) a hedge is not a substitute for the check it describes, and is
  worse than none.** The G0 document re-rated D-N1 to "chore" and *in the same
  paragraph* wrote "treat this as a claim to be re-checked — a read of one
  site is not a reading of the chain". Four sites; there are eleven; two are
  GPU out-of-bounds. **The caveat let a wrong rating ship wearing the
  appearance of caution**, and a reader reasonably discounts a hedge attached
  to a confident number.
- **(160) an approval given interactively leaves no auditable trace, and the
  document that asked must be the first one closed.** G0's outcome was
  recorded in the plan and the roadmap while the recommendation document still
  ended "What I need from you: four answers". A checker could not distinguish
  approval from write-through — every decision matched the recommendation
  verbatim, which is what both look like — and the note used
  `(C. Buahin)` rather than the project's established
  `recorded <date>, user-approved`. **Close the asking document first, in the
  existing convention.**

- **⬜ IN VALIDATION — embedded-section data loss
  (`EMBEDDED_SECTION_LOSS_HANDOFF_2026-08-26.md`).** The `[REACTION_*]`
  loss notice was gated on an optional sink that **all three production
  callers passed `nullptr` for**, so it never fired. Both C API write paths
  now forward into `ctx.warnings` — the vector
  `swmm_get_warning_count`/`_at` already expose and the GUI already reads.
  **Warn, not refuse:** the defect was the silence, not the policy, and the
  refuse-vs-warn question belongs with IO3 where `saveData()` makes it moot.
  The gate drives **`swmm_model_write_with_plugin`** and reads
  **`swmm_get_warning_at`** — the GUI's own calls — and touches the writer's
  API nowhere, because touching it is exactly how the existing coverage
  managed to certify a behaviour users never got.
  **Owed:** `IInputPlugin::write` takes a `const` context and still cannot
  surface this on success; `swmm_model_write` is fixed but ungated; the GUI
  side is untouched.
- **(161) a fix can introduce the defect it is fixing, in the opposite
  channel.** My first draft of the plugin hunk wrote the warnings into
  `last_error_` on a **successful** write — so a caller checking
  `last_error_message()` after a good save would read a warning as a failure.
  **Worse than the silence it replaced**, and caught only by re-reading my own
  diff. The lesson is not "be careful": it is that **a round fixing a
  reporting defect is operating on reporting channels, which is precisely when
  putting the wrong thing in the wrong channel is easiest.**

- **✅ Embedded-section data loss FIXED and COMMITTED `7d43a1ff`.** Both C API
  write paths forward into `ctx.warnings`; base failure reproduced exactly as
  predicted. Corpus **19/19 `.out` byte-identical and 19/19 `.rpt`
  content-identical** (differences confined to the run clock). ctest
  **177/177 ×3**. The policy call was endorsed on its merits: *refusing the
  save would strand a user with an unsaveable model and no way out, which is
  worse than a save that names what it dropped.*
  - **⛔ MY GATE HAD NEVER BEEN RUN.** `_pc_embed.inp` carried
    `[REACTION_OPTIONS]` **alone**, and a reactions config is rejected without
    at least one species — so the deck failed to open with `SWMM_ERR_PARSE`
    and the gate **died at its setup line, before reaching a single assertion
    it exists for.** Fixed by adding a `[REACTION_SPECIES]` that does not
    collide with the deck's TSS pollutant.
  - **⛔ My falsifier iv was WRONG.** Dropping the SETUP leg *and* the
    embedded section does **not** pass vacuously — it fails at `ASSERT_GT`
    (0 vs 0), because with nothing lost no warning fires. **So the SETUP leg
    is not what stands between the gate and vacuity; `ASSERT_GT` already is.**
    Worth keeping as a diagnostic, but I mis-identified which assertion was
    load-bearing.
  - **Both owed gates closed**: one for `swmm_model_write` (reverting only its
    sink leaves the first gate green — **one gate genuinely does not cover two
    entry points**, confirming falsifier ii), and one pinning the plugin's
    error channel **empty after a successful write**, so the draft defect I
    caught cannot be quietly reintroduced.
  - **Step 4 answered: the warning reaches no `.rpt` at all — blast radius
    nil, structurally, because the CLI never calls a write path.**
  - **The corpus A/B trap, restated as method:** `openswmm` resolves the
    engine through `@rpath`, so building two CLI binaries and copying them
    aside **compares a program to itself**. The two **dylibs** must be built,
    md5-confirmed distinct, and swapped in place. This is lesson 135 in
    operational form and `run_corpus.sh`'s engine-library hash exists to catch
    the naive version.
  - **Still open: the GUI half.** The engine's silence is fixed; the GUI does
    not display what it can now read, so **a user still sees nothing on save.**
- **(162) a gate that cannot open its own deck never reaches the assertions it
  was designed around — and I would not have known, because I do not run
  them.** `_pc_embed.inp` was invalid input: `[REACTION_OPTIONS]` without a
  species. The elaborate SETUP leg, the "names the section" check, the
  saved-deck assertion — none executed. **The first real assertion in any gate
  is whether its fixture is valid at all**, and that one is easiest to get
  wrong when the fixture is hand-written for a configuration nobody normally
  writes. Every gate I ship is unrun by construction; this is the round where
  that cost was total rather than partial.
- **(163) identifying which assertion prevents vacuity requires running the
  vacuous case, not reasoning about it.** I recorded a SETUP leg as
  load-bearing and predicted the gate would pass without it. Measured: it
  fails at `ASSERT_GT` instead — a different assertion was already doing the
  work. **A falsifier prediction is a claim like any other**, and mine was
  reasoned rather than run.

### ✅ GUI half of the embedded-section data loss — LANDED `040a8de` (openswmm.gui, 2026-08-27)

Committed by the concurrent session before I reached it. I came to implement
and found it done, so I **reviewed it instead** — and the review is the
contribution.

**What `040a8de` does, verified rather than trusted:**

- `SWMMVisProjectWindow::saveAs` brackets `swmm_get_warning_count` across the
  engine write and captures the **delta** — what THIS save said, not the
  cumulative history — exposed as `lastSaveWarnings()` and emitted as
  `saveCompletedWithEngineWarnings` after the save fully settles.
- One connection in `SWMMVis` routes every entry to the log panel and
  escalates the data-loss family (`"lost from this save"`) to a modal.
  **Queued**, so the modal cannot re-enter `auto-save-before-run`.
- Three gates, all driving `saveAs` itself and none calling an engine write
  directly — the engine round's own lesson, applied.

**Two claims I checked instead of accepting:**

- *"Every GUI save path funnels through `saveAs`"* — **true.** The only
  production engine-write call site in the repo is
  `swmmvisprojectwindow.cpp:1439`; every other hit is a test or a comment.
- *"Emitted last, so subscribers see the save settled"* — **true, and no
  captured delta can be stranded**: between the write and the emit there is
  exactly one `return false`, and it is the `rc != 0` branch where nothing
  was captured. "Emitted last" is only load-bearing if nothing leaves early,
  so the returns had to be enumerated, not assumed.

**⚠ One residual hole, and it is in the last save a user makes.** The queued
emit is a metacall posted to `SWMMVis`, delivered only when control reaches
the event loop. **On the quit path it never does:** `SWMMVis::closeEvent`
walks the dirty sub-windows (each running its own *"Save before closing?"*
prompt), then closes dialogs, saves settings and accepts — the app exits with
the notice still in the queue. A user who edits a deck with an embedded
`[REACTION_*]` system, quits, and answers **Save** destroys the reaction
system **with no message**. The original defect, alive in the exit path,
after both halves of the fix had landed. Closing a single window while the
app stays up is fine; it is specifically **quit**.

Fixed with `QCoreApplication::sendPostedEvents(this, QEvent::MetaCall)` after
the sub-window walk. Handoff:
`openswmm.gui/workplans/SAVE_WARNING_QUIT_PATH_HANDOFF_2026-08-27.md`.

**The premise is unmeasured and step 1 of that protocol is the measurement**,
with the explicit instruction to **revert this round if the modal turns out
to appear anyway** — a change whose justification does not survive contact
should not be kept for tidiness. The fix is safe either way
(`sendPostedEvents` consumes the event, so no double-fire); only its
*necessity* is in question.

**No automated gate, and that is the honest weakness.** The defect sits
behind a modal prompt and an application quit; the harness drives
`SWMMVisProjectWindow` directly and cannot reach `SWMMVis::closeEvent`. A
gate that flushed the queue itself would **assert the mechanism against
itself and pass on a broken build** — worse than an acknowledged manual step,
because it reports coverage that is not there.

- **(164) a fix that is correct at every site can still be undelivered at the
  moment it matters most.** Both halves of the data-loss fix landed, each
  verified, each gated — and the notice still never reached a user who saved
  on the way out, because delivery was queued into a loop that was about to
  stop. **Correct-and-emitted is not the same as seen.** The question "who
  observes this, and is anything still running when they do?" is not answered
  by any assertion about the emitting code.
- **(165) a second repo's gitignore is a second copy of the same trap.**
  `openswmm.gui/workplans/` is ignored at `.gitignore:99`, exactly as
  `plans/` is here (158). The handoff for this round exists in one working
  tree on one machine. The decision owed is now owed twice.
- **(166) a literal message string shared across two repos is a coupling with
  a gate on each side and knowledge on neither.** The GUI's modal keys on
  `"lost from this save"`; gates in both repos pin that phrase independently.
  Reword the engine's message and the GUI silently downgrades destroyed user
  data to a log line — caught only once the GUI is rebuilt against the new
  engine.

### 📋 Tracker reconciliation — 2026-08-29 (stock-take of the quality program)

Asked to take stock of the water-quality work, I checked the trackers against
the code and against `LARD_CLOSEOUT_PLAN_2026-08-24.md`. **Five rows were
stale, all in the same direction: work recorded as owed that was already
done.** Corrected in `PROGRESS.md` and the GUI plan.

| stale claim | truth |
|---|---|
| §6 "Phases 2–5 and the entire GUI track are unstarted" | Phase 5 mostly delivered; the GUI track has **eight** landed commits |
| §5 "corpus has 0 water-age, 0 heat decks" | both landed `1da1d7ca` |
| §5 "the dry-link hotstart gate never landed" | it is at `test_water_age.cpp:911` |
| GUI plan G3 "Y3b owed — the editor is unreachable" | landed `bc4e07c`; `actionEditWaterAgeSources` is in the .ui at :682/:1396 with a handler at `swmmvis.cpp:6941` |
| quality-ledger units fix "unvalidated in the tree" | validated and committed **`5b21f9a6`** (2026-08-23) |

**`5b21f9a6` is the one that matters, and it is a repeat of the 2026-08-25
failure.** The round was validated, and the result was written into its own
handoff (`QUALITY_LEDGER_UNITS_FIX_HANDOFF_2026-08-23.md:180`) and **nowhere
else** — `grep 5b21f9a6 IMPLEMENTATION_ROADMAP.md` returns nothing. Recording
it below so the fix that repaired the mass-conversion seam and the corrupted
legacy reference exists in the authority, not only in the document that asked
for it.

Also confirmed closed against the closeout plan, none of which the trackers
knew: **P1.1** the rs-ladder instrument (`6566f407`), **P1.2** the dry-hotstart
gate, **P1.3** the C-API numeric audit (`22e55228`).

- **(167) a tracker that is only ever corrected downward will drift upward on
  its own.** Every one of these five rows overstated what was left. A round
  ends by writing its result somewhere; if that somewhere is the handoff that
  requested it, the authority never learns, and the next planning pass budgets
  work that is already in the tree. **The handoff is where a round asks; the
  roadmap is where the program remembers** — and this program has now paid for
  that distinction twice (see the 2026-08-25 X/Y/Z blindness).
- **(168) "verify before implementing" applies to the plan, not just the
  code.** In one sitting I nearly re-implemented the GUI half of the
  data-loss fix, then Y3b, then the dry-hotstart gate, then P1.1 and P1.3 —
  five rounds of duplicate work, each stopped only by reading the tree before
  writing to it. **The cost of checking is one grep; the cost of not checking
  is a whole round plus a merge conflict with whoever did it first.**

### ✅ Reaction-expression editor gate — VALIDATED (gui `11f8ea5` + `99fe650`, 2026-08-29)

Gate ii's open question is **answered: the pollutant leg passes.** `TSS`
resolves as an operand in PIPE scope, so all four model families the completer
advertises are grammar-valid, and gate ii now keeps them that way. Suite
177/177 with the new TU; nothing else moved.

**Two of five cases failed at `11f8ea5`, and the fault was mine.** I probed
every completer word as `AS3 = <word>` and set `AS3 = -0.5 * AS3` — but **the
reaction grammar has no assignment token.** A row is `RATE <species> <expr>`;
`swmm_reaction_validate_expression` takes the expression *alone*, and every
real consumer of the widget already hands it that. The engine's `unexpected
character '='` was correct. `99fe650` fixed the **test**, not the engine.

I imported `C = ...` from `TreatmentExpressionEdit`, whose grammar genuinely
has an assignment — the sibling I had explicitly decided *not* to mirror, in a
handoff arguing at length that the two editors' vocabularies work differently.
I made the structural argument and then copied the surface form anyway.

**Falsifier iv's prediction was wrong, and splitting it produced the better
answer.** I predicted dropping the `engineSourced` exclusion would still pass,
"showing both excluded families are tautological." It **fails on `ABS`**:
*"function 'ABS' needs '('"*. Measured separately: excluding only functions
passes 5/5. So **hydvars are the tautology I described; functions are excluded
on GRAMMAR** — a bare function name is not an operand at all. The test was
right; my justification for it was half wrong. Comment precision corrected in
the gate and in §2 of the handoff, per the checker's note.

Falsifiers i–iii bite as designed (empty completer / stale-snapshot on `NH2CL`
only / `ZZQQ` named). Falsifier v (a lying validator) stays unclosable from the
GUI side, as §6 conceded.

- **(169) deciding not to mirror something does not stop you copying it.** The
  handoff argued explicitly that the treatment editor's vocabulary model does
  not transfer — and I still imported its `C = <expr>` assignment syntax into
  a grammar that has no assignment. **A structural argument about why two
  things differ offers no protection at the level of syntax**, which is copied
  by hand, one line at a time, from the file open next to you. Lesson 144
  again, one layer down: I read the *contract* of the validator call and
  copied the *shape* of its argument.
- **(170) one exclusion, two reasons, is a comment that will be "fixed" into a
  bug.** Both excluded families looked alike, so I wrote one justification for
  both; measurement split them (tautology vs grammar). Had the checker not run
  falsifier iv, the next reader deleting the "redundant" exclusion would have
  seen `ABS` rejected and reported a widget defect that does not exist.
  **When one guard covers two cases for two reasons, the comment must name
  both or the guard is a trap.**
- **(171) a syntax-only handoff's account of the base behaviour is a claim,
  and the cheapest one to test.** P1.4 was written to fix "a negative source
  did nothing, silently"; base REFUSED the row at parse, so the round's own
  gates could not open their decks, patched or not. Running the gates at base
  (protocol step 1) found it in one minute; the handoff had reasoned its way
  past the parser it was editing. The `std::max` it removed guarded a
  different path (TIMESERIES) than the one it described.
- **(172) the shared tree cannot host an A/B.** Peer edits to `DynamicWave`
  landed between the base snapshot and the patched build and every ARD run
  died with `std::bad_alloc` — indistinguishable from the round's defect
  until a binary/dylib cross-swap and `find -newer` pinned it. Lesson 71
  said "isolated worktree" for ctest counts; it applies to every base-vs-
  patched number, including the corpus. `git worktree add --detach HEAD`
  plus the four files under test is the only A/B that means anything while
  other sessions are live.
- **(173) observe the cell you clamp.** Gate 2's non-negativity leg watched
  the downstream conduit and falsifier ii did not bite on it: the extracted
  conduit's projection carries the sign, but the node hand-off downstream
  never propagates a negative donor, so C5 read 0 while C3 sat at −1.32. A
  physics leg on the wrong element is a count leg wearing a physics name.
- **(179) a bit-identity round is only as strong as the deck that would
  move.** H7a's `np == 0` case had no corpus coverage; the identity on the
  age-only deck meant nothing until falsifier iv (an under-sized layout)
  moved exactly that deck. Pair every "nothing changed" claim with the
  falsifier that shows the instrument can see the change.
- **(180) HEAD can be red without anyone's changeset being wrong.** The
  develop merge left `swmm6_rel` non-compiling (`target_sc`, four undeclared
  LID methods) and five gates failing, all repaired only in a peer's working
  tree. A checker that starts from "build HEAD" finds this in one step; a
  checker that starts from the shared tree never sees it. Isolated worktree
  first, then read what it needed borrowed to compile.
- **(184) STANDING FIGURE (2026-08-30): `8f9f164d` — 180 registered, 175
  passing, 5 failing ×3, no flakes, all five also fail standalone.** Four
  (`water_age_lid` ×2, `heat_watershed`, `heat_lid`, `transport_dt_reference`)
  pass at `8c8faa3c` and fail after merge `a38f0c0b` — brought by the remote
  LID PR #103 (`5f6a2ba5`/`e2295827`): LID storage no longer drains through
  the underdrain; the merge repair's receiver choice is NOT the cause (probed).
  **H7b waits behind that.** `fv_tpa_closure` (4 of 9) was red at its own
  introducing commit `47c00ae3` (#156) with O-6 absent — inherited, O-6's to
  fix. Corpus reference at `8f9f164d` stored under
  `tests/output/rebaseline_8f9f164d/corpus/`. `REBASELINE_HANDOFF_2026-08-29.md`.
- **(185) the four LID gates, attributed (2026-08-30):** drain-rate ratio
  measured 12 470.8× = 43 200/√12 exactly (`heat_lid` deck; old head 50 ft =
  inches read as feet; runoff continuity 96.5 % → −0.067 %). **But restoring
  the drain line alone greens NOTHING** — `water_age_lid`, `heat_lid`,
  `transport_dt_reference` are fitted to the WHOLE pre-#103 units regime (19
  parameter conversions; falsifier B greens all three), not to one line.
  `heat_watershed` is a different defect: the merge's `SWMMEngine.cpp` books
  an own-subcatchment underdrain as `runoff` instead of run-on **and carries
  no temperature or age on that branch** (falsifier D greens it) — H5b's pair
  invariant broken, seventh flow-knows/quality-doesn't. **H7b stays blocked
  behind that.** `LID_UNDERDRAIN_GATES_HANDOFF_2026-08-30.md` CHECK RECORD.
- **(186) a falsifier that fails is the round's most valuable output.** The
  handoff predicted "restore one line → five gates green" as the proof the
  gates were fitted to the defect; it went red, and splitting the restoration
  (units regime vs clamp vs routing semantics) turned one story into three
  attributions, one of them a live heat defect. Run the predicted-pass
  falsifier before writing the narrative, not after.
- **(187) LID fix round LANDED 2026-08-30: `e07d66e5` (void-ratio validator
  capped a RATIO at 1.0 — 75 % voids was unexpressable) + `082dd7c1` (decks
  re-expressed in user units per their own @warning plan; dt bands re-pinned
  to measured floors) + `ee7494ea` (target-less underdrain → outlet node per
  legacy lid.c:1215, paired temperature+age at the node seam, drain volume
  counted once).** Under B1 sat two more defects: `ext_inflow` is CLEARED
  after stepRunoff adds to it, so drain-to-node water NEVER reached the
  network (new `nodes.lid_drain_inflow` channel); and the runoff ledger had
  no RUNOFF_DRAINS term or LID Drainage row. Standing figure now **179/180
  at `ee7494ea`** (fv_tpa_closure, the peer's). Corpus 20/20 + 0/20 rpt vs
  the 8f9f164d reference. **H7b UNBLOCKED.** The −44 % runoff continuity on
  the outfall-recirculation deck is PRE-EXISTING (flooded volume leaves the
  loop; identical at base) — a ledger question for its own round.
  `LID_FIX_ROUND_HANDOFF_2026-08-30.md` CHECK/IMPLEMENTATION RECORD.

### P1.4 — implementer's record of the corrections (2026-08-29, `4b26aa50`)

The checker's findings, recorded from the implementing side because each is a
mistake of mine with a reusable shape.

**The round's stated defect did not exist.** I claimed a negative
`[TRANSPORT_SOURCES]` row was *silently zeroed* by
`src_now_[i] = std::max(0.0, r)`. It was **refused at parse** — `ArdConfig`'s
shared boundary/source parser rejects a negative numeric field, and the open
fails. That `std::max` was unreachable for VALUE rows. My patch never touched
the line that actually blocked them, so **all three of my own gates still
failed with the patch applied** — the decks could not open.

I read `resolveArdTransportRows` and the apply loop; I never read the parser
that fills them. **I entered the chain in the middle and described it as the
chain.** The hint was in the file I was editing: the existing source gate uses
positive values only, which is what a parser that refuses negatives produces.

**My gate watched a point where its own falsifier could not bite.**
`OverExtractionClampsAndStaysNonNegative` observed C5, downstream of the
extraction. Remove the clamp and C5 stays non-negative anyway — the node
hand-off never propagates a negative donor — so the gate would have passed
forever over deleted code. On C3, the extracted conduit, the falsifier reports
**c3_min = −1.32**. The name promised non-negativity; the assertions checked
counts and warnings.

**And the band was seven orders too loose.** I reused gate 3's 15 % by analogy
on a quantity that lands at **3.00000007** against an analytic 3.0. Now 1 %.

Also missing: `swmm_engine_initialize` in gate 2 (LIFECYCLE error 6) — I
invented a start/step/end sequence rather than reading one.

**Method note worth keeping:** the checker ran the A/B in an **isolated
worktree** because peer edits to DynamicWave landed mid-run in the shared tree
and produced a `bad_alloc` in every ARD test *that looked exactly like a P1.4
defect*. Lesson 71's trap, live: a measurement from a shared tree is not
attributable to the changeset under test.

- **(174) reading a value's consumer is not reading its provenance.** I traced
  `src_now_` forward from where it is used and never traced it back to where
  the deck's text becomes a number. Every claim of the form "X is silently
  dropped" is a claim about the WHOLE path from input to effect, and it is
  false if anything upstream refuses X outright. **Start at the text, not at
  the variable.**
- **(175) a falsifier that cannot bite at the observation point proves the
  observation point, not the code.** The clamp gate watched downstream of the
  clamp, where the defect is masked by an unrelated non-negativity property.
  **Where to look is part of the assertion**; picking it by convenience —
  "C5 is what the sibling gate reads" — silently narrows what the gate can
  ever detect. The give-away was available before running anything: the gate's
  NAME described a property its assertions did not mention.

### P1.4b — the per-clamp runtime warning is retired (2026-08-29, awaiting validation)

**User decision, recorded 2026-08-29, user-approved:** drop the D-NS1 per-clamp
runtime warning; keep the end-of-run summary.

The P1.4 check measured the warning's actual behaviour: **every extraction deck
clamps during fill** — 108 clamps on a deck extracting 40 % of its inflow, none
of them indicating a problem. Applied to the **shared seam** (node, age, ARD
cell) so the three engines stay identical rather than drifting apart to spare
two of them an edit.

`runtime_warned` → `first_clamp_recorded`; it no longer gates a warning, only
the `first_node` capture. Two gate rows deliberately flipped from "fires" to
"is gone", the ARD one strengthened to pin that the clamp still reaches a
user-visible channel at all. Handoff:
`P1_4B_CLAMP_WARNING_CONTRACT_HANDOFF_2026-08-29.md`.

**Deliberate non-fix:** the fill-clamping itself is untouched. Suppressing it
needs a "still filling" predicate, and **"dry" is not one predicate**
(lesson 143) — its own round if ever wanted.

- **(176) measuring how often a warning fires is part of designing it.** D-NS1's
  per-clamp warning was reviewed and approved when it landed, and it was wrong
  in a way no review could catch: **its defect was a frequency, and frequency
  is only visible from a run.** The same is true of every notice keyed to a
  condition whose base rate nobody has counted — "warn when X" is not a
  finished design until someone knows how often X happens on a correct model.

### ✅ P1.4b VALIDATED and COMMITTED `0e73f7ea` (2026-08-29)

Both flipped rows fail at base on **all three engines** and pass patched
(4/4, 13/13). Falsifier i (restore the node seam alone) fails only the
three-engine row; ii (cell seam alone) only the ARD row — **the two gates
cover different seams, as claimed**. Falsifier iii (delete the summary) fails
both, confirming the round did not remove the last observer. Over-extraction
decks now report *"first at element index 2"*. Corpus **20/20 `.out`
byte-identical, 0/20 `.rpt` moved** — the predicted nil blast radius held.
ctest 176/177 ×3, the single failure pre-existing at HEAD.

Test renamed per the checker's note:
`OverExtractionClampsWarnsAndStaysNonNegative` →
`...ClampsSummarizesAndStaysNonNegative`. The other `*Warn*` names describe
parse warnings, which still fire.

- **(177) a record written before the round's last step will assert that step.**
  The check record opened "landed on `swmm6_rel`" while the covering message
  said the commit was blocked; the tree agreed with the message. The
  verification was complete and correct — only the landing had not happened.
  **Write the outcome of a step after taking it, not while intending to.**
  This is the staleness problem of this whole program in miniature, arriving
  inside a single document.
- **(178) an artefact you cannot delete becomes evidence someone else has to
  interpret.** My sandbox orphans empty `.git/*.lock` files because the mount
  permits create and rename but denies unlink. A checker correctly identified
  the holder pattern, correctly refused to remove a lock of unknown
  provenance, and lost a round-trip — because the litter was mine and only I
  knew that. **Clean up what you cannot delete by renaming it aside in the
  same call that creates it**, or leave a note saying whose it is.

### ✅ H7a VALIDATED and COMMITTED `f31efd63` (2026-08-29)

Bit-inert as claimed: corpus 20/20 `.out`, 0/20 `.rpt`; water-age + five LARD
suites unchanged (49 gates). Falsifier i **passes**, as predicted — the
equivalence holds today and the round's value is what it enables.

**The checker made §5.3 stronger than I wrote it.** I asked for the `np == 0`
(age-only) case because an off-by-one would show there first, and no corpus
deck covers it. They then showed the identity is **not vacuous**: falsifier iv
(under-sized layout) moves exactly that deck plus the age+pollutant one and
fails two LardAge gates. **An identity result means nothing until something is
shown to be able to break it** — asking for the case was right; proving the
case is live is what made it evidence.

### ⛔ TWO FINDINGS THAT OUTRANK H7b (2026-08-29)

**1. `swmm6_rel` DOES NOT COMPILE from a clean checkout.** At HEAD
`a38f0c0b` (the develop merge): `SWMMEngine.cpp:2058` references `target_sc`,
which does not exist on this branch, and `LID.cpp` defines four
`LIDSolver::total*Volume()` methods `LID.hpp` never declares. A peer session
holds uncommitted repairs for both; the H7a check had to **borrow unstaged
work to build either side of its A/B**.

**This is worse than a red build.** It means the *baseline* for every
subsequent round exists only in one working tree. Lesson 71 says a measurement
from a shared tree is not attributable; here there is no attributable tree to
measure from at all. **Nothing further should be validated until those repairs
are committed.**

**2. The standing ctest figure is stale — 180 registered, 5 failing at HEAD**
(`water_age_lid`, `heat_watershed`, `heat_lid`, `transport_dt_reference`,
`fv_tpa_closure`), every one failing identically against the base dylib, so
they are the merge's (LID/heat/TPA). The old `2d_infil` failure now passes.
**"177/177" is retired**; re-baseline once finding 1 is fixed.

### D-H7b1 — temperature DOES disperse under LARD (decided 2026-08-29)

The open question from H7a §6, settled **by precedent rather than by physics
argument**: `dispersionSolve` loops `ns = state_->n_species`, so the **ARD
engine already disperses the temperature row** with the same coefficient as
every other row. Excluding heat from RWPT under LARD would manufacture an
ARD/LARD divergence — the cross-engine drift the shared-seam discipline exists
to prevent.

So H7b includes temperature in RWPT, and the bypass warning is **deleted
outright rather than narrowed**, because heat will be fully supported:
transported *and* dispersed. (Had ARD excluded it, the answer would have been
the opposite, and my physics reasoning about Taylor dispersion being a property
of the flow field rather than the tracer would have been the wrong tool —
it argues for inclusion, but what settles it is what the sibling engine does.)

- **(181) when two engines must agree, the question "what is correct?" is
  downstream of "what does the other one already do?"** I was ready to decide
  heat-dispersion on physics. The decisive fact was one grep: ARD disperses all
  rows. Physics would have given the same answer here by luck; on the next such
  question it may not, and the divergence would have been mine.

### ✅ Blocker 1 CLEARED — `8f9f164d` (2026-08-29): swmm6_rel compiles again

The peer session's two merge repairs (`target_sc` → `g.drain_subcatch[uu]`;
the four undeclared `LIDSolver::total*Volume()`) were found complete and
uncommitted in the shared tree and are now committed. `g++ -fsyntax-only
-std=c++20` over the real include tree: **0 errors for the whole
`SWMMEngine.cpp` TU and for `LID.hpp`.**

**Blocker 2 (re-baseline ctest) still stands** — 180 registered, 5 failing at
HEAD. It needs a build, so it belongs to the checking agent, on top of this
commit.

**⚠ I committed unreviewed work and had to undo it.** My first attempt
(`0cfc2242`, since replaced) staged `src/engine/core/SWMMEngine.cpp` **by
path**. Between reading its diff (6 lines, two repair hunks) and running
`git add` minutes later, the peer added their in-flight `REPORT_SIGNED_HEADS`
work (#156 O-6) to the same file — **two further hunks, a behaviour change,
inside a commit whose message said "Neither is a behaviour change."** Undone
with `reset --soft`, re-staged **by patch** to the two repair hunks alone, and
O-6 returned to the working tree uncommitted where its author left it.

- **(182) in a live shared tree, `git add <path>` commits the file's state at
  the moment you run it, not the state you reviewed.** The gap between reading
  a diff and staging it is not zero, and a concurrent session does not pause
  for your review. **Stage by patch and re-verify the patch still applies** —
  the protocol I had written down, skipped because the diff "looked small".
  The tell was available for free: the staged `--stat` said 22 lines where the
  reviewed diff said 6, and I read past it into the commit.
- **(183) an authorship note in a commit message is not a substitute for
  reviewing what is in the commit.** I was careful to attribute the repairs to
  the session that wrote them and careless about whether the commit contained
  only those repairs. Getting the credit right while getting the contents
  wrong is the more embarrassing half.

### 🔍 The LID underdrain "regression" is a REPAIR — diagnosis 2026-08-30

The re-baseline attributed four heat/LID failures to remote PR #103
(`5f6a2ba5`) and left open whether it broke underdrain return or the gates
encoded a behaviour it changed. **Neither: the gates encode a BUG it fixed.**

At `8c8faa3c`, `drain_coeff` was seeded from the deck in **in/hr** and returned
from `getDrainRate` **as ft/s**. `UCF(RAINFALL)` is 43200, so the underdrain
ran **43 200× too fast** — and `offset`/`hOpen`/`hClose` were seeded in inches
while `h` was in ft, so the head comparisons were wrong too. PR #103 made all
of it dimensionally correct.

**The seeding line for `drain_coeff` is byte-identical across the PR.** Only
the comment and the point of use changed — which is why the defect is invisible
to a diff-reading sweep, and why the old comment (*"drain_coeff is already in
ft/s"*) was simply never true.

**This is `landuse.c` one subsystem over**: a units defect in the reference
implementation with gates fitted to it, so repairing it presents as a
regression. Handoff: `LID_UNDERDRAIN_GATES_HANDOFF_2026-08-30.md`, which
refuses the "restore the old drainage" option outright and treats each gate as
a horizon/premise question before a numbers question.

`fv_tpa_closure` is unrelated and pre-dates both merges (`47c00ae3`).

- **(185) when a fix breaks tests, ask first whether the results got MORE
  physical.** Both times this program has met calibrated-to-a-defect gates
  (`landuse.c` `>=`, now the 43 200× underdrain) the tell was the same: the
  "regression" moved results toward reality. The reflex to protect green tests
  points exactly the wrong way here — the tests are the thing that is wrong.
- **(186) a storage convention lives in three places — the seeding, the use,
  and the comment — and only two of them are checked by anything.** The
  `drain_coeff` assignment never changed; the comment asserting its units was
  false for as long as it existed, and the use was written to match the
  comment. **A diff of the assignment shows nothing on the round that repairs
  it.**
- **(187) evidence gathered in a defective regime does not become wrong, it
  becomes uncited.** Every H5b/A4 result taken through a LID underdrain was
  measured against 43 200×-fast drainage. Nothing is known to be false; those
  measurements simply cannot be cited as if the regime were correct.

### D-H5c1 — own-subcatchment underdrain discharge: the legacy tie-break (2026-08-30)

The check asked for a decision, naming `lid_addDrainInflow` as the tie-break.
It is actually its sibling, and the answer is unambiguous.

**`lid_addDrainRunon` (`src/legacy/engine/lid.c:1554`):**
```c
k = lidUnit->drainSubcatch;
if ( k >= 0 && k != j )          // own subcatchment EXCLUDED
{
    q = lidUnit->oldDrainFlow;
    subcatch_addRunonFlow(k, q);
    ... pollutant loads ...
}
```

**Legacy refuses to route a LID's underdrain back to its own subcatchment as
run-on.** The `k != j` guard is explicit, and it takes the pollutant load with
it. So the merged code's choice is *not* run-on, and routing it as run-on
would be a divergence with no formulation error behind it.

**But `runoff +=` is only right if it is a REPORTING addition.** Legacy adds
drain flow to runoff at **`subcatch.c:897-900`**, under the comment *"add any
LID drain flow to **reported** runoff"*, with drain tracked separately in
`VlidDrain` and the file's own header noting drains are kept apart *"even
though [they] can be routed elsewhere"*. That line is reporting, not routing.

**So the question for the fixing round is precisely: is our `runoff +=` a
routing addition or a reporting one?** If routing, we create water legacy does
not — and the fix is to drop the own-subcatch routing entirely (matching
`k != j`) while keeping the reported-runoff line. **Whichever channel wins, the
heat/age pairing must be restored**: the merged branch carries no
`addRunonTemperatureAt`/age pairing at that site, which is H5b's pair invariant
broken, and that is a defect independent of the routing question.

**Corrected attribution, from the check's falsifier split:** three gates
(`water_age_lid` ×2, `heat_lid`, `transport_dt_reference`) encode the **whole
pre-#103 LID units regime** — thickness, three ksat conversions, void ratio,
init_sat, drain delay, offsets — **not** the drain line alone, which is why my
§5.4 falsifier failed as written. `heat_watershed` is a separate live defect.
The measured drain ratio is **12 470.8× = 43 200/√12**, not 43 200: the old
regime also ran a 12× deeper column (inches read as feet), and runoff
continuity on that deck moves **96.5 % → −0.067 %**.

- **(188) a falsifier that restores ONE line of a multi-line change tests that
  line, not the change.** I wrote "restore the old drain return and the gates
  go green" as the round's whole-round proof. PR #103 converted **19** unit
  sites; restoring one left every gate red, and only the full restoration
  separated the three units-regime gates from the one semantics defect.
  **When a change has many parts, the falsifier must be able to restore them
  in groups** — otherwise a red result means "not that line" and is mistaken
  for "not that cause".
- **(189) an exact-looking ratio is a hypothesis about mechanism.** I predicted
  43 200× from the rate-units defect alone. Measured 12 470.8× — which is
  43 200/√12, and the √12 is a *second* defect (head in feet read as inches)
  interacting through `pow(h, expon)`. **The discrepancy in a confirmation is
  where the rest of the story is**, and rounding it off as "within a couple of
  orders, close enough" would have hidden a whole defect.

### ✅ H7 COMPLETE — H7b landed `deb42172` (2026-08-30)

Heat under LARD is functionally done. Temperature rides the segments through
every generic stage, sourced from `node_temp_vol_in`, published to
`heat_state`; `publish()` joined `rowLayout()`, closing H7a's fourth
layout-aware site; the bypass warning is **deleted, not narrowed** — the row
takes RWPT dispersion at the solute coefficient, per D-H7b1. Corpus **21**
(`heat_lard`), ctest **180/181** (only the peer's `fv_tpa_closure`).

**Three exemptions, each with its own falsifier** — temperature is exempt from
the D-NS1 clamp (sub-zero °C is legal), does not age, does not decay. Naming
them as exemptions rather than letting them fall out of the code is what makes
the reserved rows' differing laws legible.

**Two of the sequence's assumptions were wrong**, and the gates record it
rather than working around it: **hotstart has no temperature substrate in any
engine** (the record carries age, no temperature field — gate 3 pins the
re-seed and fails the day the field appears; format widening owed to step 9),
and the **empty-slab temperature hold has no reachable observer** — a
state-zeroing falsifier stayed green, so it is recorded rather than gated
vacuously. **Declining to write the unfalsifiable gate is the right call**
(lesson 163's family).

**Two findings owed onward, both outside H7:** `[POLLUTANTS]` Kdecay applied as
**1/second here vs legacy's 1/day**, and an `[INFLOWS]` row naming a pollutant
declared later in the file **silently loads zero**. The first is a parity
defect on every decaying deck; neither is H7's to fix.

- **(190) the tree can change during your own investigation, not just before
  it.** I read `LagrangianSolver.hpp` twice while working out how to land the
  patch; between the two reads line 155 went from `// H7b will add:` to the
  real assignment — the worker's background poll had landed `deb42172`
  mid-analysis. I was three commands from applying a patch for work already in
  HEAD. **"Check before acting" is not a step you complete; it is a condition
  that expires**, and in a live tree it expires while you are reading.
- **(191) a `git diff`-generated patch omits new files, and the round it
  describes is exactly the round that adds them.** `h7b.patch` carried 5
  tracked files; the new `test_lard_heat.cpp` and `heat_lard.inp` were absent,
  so applying it alone would have registered a test and a corpus deck that did
  not exist. It happened not to matter — but a patch handed over as "ready to
  apply" needs `--binary`/`-N` or an explicit file list, or it is a build break
  waiting for whoever trusts it.

### Step 3 rescoped — the heat C API already exists (2026-08-30)

`FINALIZATION_SEQUENCE_2026-08-29.md` step 3 says *"`openswmm_heat.h` does not
exist"* and asks for it to be designed. **H6a built it** — 17 exported
functions across `[HEAT_FLUXES]`, `[RADIATIVE_FLUXES]`, `[SOLAR_RADIATION]`
and `[CLOUD_COVER]`, currently untracked and unvalidated. Writing step 3 as
specified would have produced a second, conflicting heat API.

**The one gap is `[HEAT_SOURCES]`** — the per-source inlet temperature table,
which is precisely what G4g must round-trip. Handoff:
`STEP3_HEAT_API_GAP_HANDOFF_2026-08-30.md`, blocked behind H6a's validation so
that this round's failures are attributable to this round.

**`[HEAT_METEOROLOGY]` does not exist.** The GUI plan's G4g spec names it as a
section to round-trip; the authoritative list (`HeatComponent.cpp:522-527`) has
five sections and that is not one of them. RH and wind live in the existing
`[TEMPERATURE]` climate section (`HydrologyHandler.cpp:232`) and already have
UI. G4g's spec is corrected in the handoff.

- **(192) the fifth near-duplication was stopped by a human sentence, not by
  any check I run.** "Another agent has done some work on the heat" is what
  made me read H6a before writing. My own protocol says verify the plan before
  implementing (168), and I do it — but I verify *the claim I am about to make
  about the code*, not *whether someone else is already making it*. Those are
  different sweeps, and only the first is habitual. **Before a round that
  creates a new file, grep for the file.** It costs one command and it is the
  cheapest check in this program.

### Step 3 IMPLEMENTED — `[HEAT_SOURCES]` C API (2026-08-30, awaiting validation)

11 functions + `SWMM_HeatSourceKind` appended to H6a's `openswmm_heat.h`;
6 gates in `test_heat_sources_api.cpp`. Syntax-clean (0 errors, C++20).
Blocked behind H6a's own validation. Record:
`STEP3_HEAT_API_GAP_HANDOFF_2026-08-30.md`.

**⚠ The round's real finding is not the API.** `swmm_model_write` emits
`[PROCESS_COMPONENTS]` with the config= PATH and never rewrites the config
file's CONTENT — there is no per-component `saveData()` (IO3, still owed). So
**every `[HEAT_SOURCES]` edit made through the API or the GUI is silently lost
on save**, and unlike the embedded-section case it is NOT warned: the writer's
warning covers embedded sections only, and its advice ("move them to an
external component config file to keep them") **is false for anything edited
through the software.** Hand-edit the file and it persists; edit it in the app
and it does not.

**G4g is therefore blocked on component-config serialization, not on the API.**
An editor built on this today would let a user edit a table that vanishes on
save. `NodeOverrideEditsDoNotSurviveASave` pins the loss so it is observed
rather than discovered, and **must fail the day IO3 lands.**

- **(193) "the API exists" and "the feature works" are separated by whoever
  writes the file back.** I scoped step 3 as the missing read/write surface
  and it was — but a table you can read and mutate and cannot persist is not
  a feature, it is a trap with a nice interface. **Before building an editor
  on a config surface, follow one edit all the way to disk.**
- **(194) a warning\'s ADVICE ages separately from its trigger.** The
  embedded-section warning is correct about when to fire and wrong about what
  to do: "move to an external config file to keep them" was true when only
  hand-editing existed, and became false the moment an API could edit that
  file. **A warning that tells the user what to do is making a claim about
  the rest of the system**, and nothing re-checks it when the rest of the
  system changes.

### ✅ H6a VALIDATED + step 3 COMMITTED `803d5cbc` — one round (2026-08-31)

The combined heat changeset landed after full validation in an isolated
worktree: base `deb42172` 180/181 → patched 182/183 ×3 → re-validated on
`72474eb8` (the DW-TPA landing) at **184/184 ×3 — the suite's first fully
green figure since the census began**; corpus 21/21 byte-identical incl.
`heat_parity` and `heat_lard`. Falsifiers i–iv, vi bite; v (re-derived
precedence) passes and is the recorded unobserved coupling. Records: H6a
handoff §6, step-3 handoff CHECK RECORD, artifacts
`tests/output/step3_heat_api/`.

- **(195) a suite that has never executed can be wrong in the FIXTURE, and
  its refusal gates then pass VACUOUSLY.** H6a's 18 gates all drove decks
  whose `[PROCESS_COMPONENTS]` row lacked `config="…"`: 7 open-expecting
  gates failed, and the refusal gates "passed" because the open failed on
  the malformed row, not on the refusal under test. A refusal gate is only
  evidence after you have watched its deck OPEN with the refusal removed.
- **(196) check the FORM of a range test before repeating a claim about
  NaN.** §5.6 said `parse_celsius` shared the NaN hole; it never did — its
  test is the conjunctive ACCEPT form (`v >= lo && v <= hi`), which NaN
  fails. The reject form (`v < lo || v > hi`) is the vulnerable one, and
  `frac()` had it — a live deck/API disagreement, now fixed. The two forms
  read as synonyms and are not.
- **(197) secondary sources disagree about published coefficients; verify
  against a DIFFABLE implementation, not a rendering.** One reference page
  printed Bird's ozone exponent as −0.3035 and Ba as 0.84; pvlib's
  NREL-faithful source carries −0.3034 and 0.85 — matching the code. Had
  the check trusted the rendering, a correct transcription would have been
  "fixed" into a wrong one.

### IO3a IMPLEMENTED — components write their own config files (2026-08-31, awaiting validation)

Step 3's finding is fixed at the seam. `ComponentConfigSave` is the inverse of
`ComponentConfigApply`; `InpWriter` calls it before the carry-alongside copy;
an **empty return declines** and falls back to copying, so components adopt
saving one at a time with no intermediate state losing data. Heat and
reactions adopt now; water age and ARD keep copying (IO3b). Handoff:
`IO3A_COMPONENT_SAVE_HANDOFF_2026-08-31.md`.

**"IO3 is complete" was half true, and the naming hid it.** The 2026-08-17
round delivered IO3's *carry-alongside copy*; its *serialization* half never
existed, while `InpWriter`'s comment and the roadmap both said "until IO3" as
though one thing were pending. Task lists marked it done.

`NodeOverrideEditsDoNotSurviveASave` — written yesterday to pin the loss, with
a message instructing its own replacement — **failed as designed** and is now
`SourceEditsSurviveASaveAndReopen`.

- **(195) the capability, the intent, and the call site are three separate
  things, and two of three is silence.** Reactions had a complete canonical
  serializer (E-C3) and `InpWriter` had a comment stating that components own
  their own files. Both were right; nothing connected them, so edits vanished
  for months. **When a subsystem "doesn't do X", check whether it cannot or
  whether nobody asks it to** — this program has now found the same shape
  three times (the writer's warning sink, `getDrainRate`'s units, this).
- **(196) a serializer that writes defaults corrupts as surely as one that
  drops edits.** Emitting all seven `[HEAT_SOURCES]` rows at 20 °C would make
  every saved model look explicitly configured. `configured_source` exists to
  prevent exactly that, and the round-trip gate asserts the *absence* of an
  invented row alongside the presence of a real one. **"Round-trips" means
  neither losing nor inventing.**

### ✅ IO3a COMMITTED `4738bca9` — components write their own config files (2026-08-31)

Heat + reactions adopt the new `ComponentConfigSave` hook (empty return =
decline → copy fallback; adoption one component at a time, no intermediate
state loses data). Gates-on-base quoted the loss (14.5 vs 31.0); patched
184/184 ×3; corpus 21/21. Check round: implemented §6's decline-condition
recommendation (`hasUnrenderableSections` — H6a sections decline rather than
truncate, pinned by gate) and repaired a VACUOUS idempotence gate. Record:
IO3A handoff CHECK RECORD; artifacts `tests/output/io3a_component_save/`.

- **(198) a gate that reads its evidence AFTER both acts compares a thing
  with itself.** `SaveIsIdempotent` slurped the config after both saves —
  vacuously equal forever. Idempotence evidence must be captured BETWEEN
  the acts; falsifier v (timestamp) is what proves the repaired gate
  observes, and it passed against the broken spelling.
- **(199) a serializer that renders a SUBSET of its component's sections
  must decline on the complement, not write what it knows.** Rendering only
  [HEAT_SOURCES]/[HEAT_FLUXES] over a file carrying [RADIATIVE_FLUXES]
  would have truncated H6a's config on every save of every H3+ deck — the
  handoff flagged it, the check round implemented its recommendation, and
  the gate pins both the preservation and its cost (the IO3b gap).

### ✅ IO3a VALIDATED and COMMITTED `4738bca9` (2026-08-31) — implementer's record

8/8 gates, **184/184 ×3** (the census is fully green again), corpus 21/21
byte-identical including `heat_parity` and `heat_lard`. Base failure quoted
exactly as predicted: *14.5 where 31.0 was asserted* — the original file copied
back over the edit.

**The check closed the hole my own §6 flagged, and it was worse than I
priced it.** I wrote that `saveHeatConfig` renders no H6a section and declines
only when *nothing* is configured, recommended making the decline condition
"any section I cannot render is present", and **did not implement it**. The
checker did. Without it **every H3+ deck's radiative configuration would have
vanished on its first save** — this round would have shipped a new data-loss
path while curing another. Naming a hole is not closing it; I had the right
answer written down and left it as a note.

**And I shipped a VACUOUS gate — in the round where I was arguing about
vacuity.** `SaveIsIdempotent` slurped `_hs_idem.heat` twice *after both saves
had already run*, comparing a file with itself: trivially true, observing
nothing. The checker rewrote it to capture between saves. **My own falsifier v
(append a timestamp) is what exposed it** — it passed against the broken
spelling, which is precisely the signal a falsifier exists to give.

- **(200) a gate that reads the same artefact twice compares nothing, and it
  reads like a comparison.** `EXPECT_EQ(slurp(p), slurp(p))` has the shape of
  a round-trip check and the content of a tautology. **An idempotence gate must
  CAPTURE between the two operations** — if both reads happen after both
  writes, there is only ever one file. I wrote this in the same file where I
  refused to write a tautological drift guard, so knowing the failure mode is
  not the same as recognising it in one's own hands.
- **(201) recommending a fix in the "known gaps" section is not shipping it.**
  §6 named the data-loss hole, diagnosed it correctly, chose between two
  remedies and said which was safer — then stopped. A gap section is for what
  the round DECIDED not to do; putting a live data-loss path there, with its
  own fix already worked out, was a handoff doing the reasoning and declining
  the work. **If the fix is one condition and the cost of omitting it is data
  loss, it belongs in the round.**

### ✅ IO3b COMMITTED `23c1ddfb` — the heat renderer covers every section (2026-08-31)

Guard deleted per the handoff's preferred ending; static_assert size pins
replace it (falsifier iv: growing RadiativeConfig breaks the build).
184/184 ×3; corpus 21/21. **G4g fully unblocked.** Water age/ARD → IO3c
(unit-bearing configs: seconds-vs-hours fixed-point question). Record: the
IO3b handoff's IMPLEMENTATION + CHECK RECORD.

- **(200) a renderer and its parser drift in the KEY SPACE, not just the
  value space.** The first spelling emitted `LW_REFLECTION`; the parser's
  key is `ATM_LW_REFLECTION`. Nothing but a full write→reopen→read gate
  catches that class — the API layer never sees deck keys. The
  field-by-field gate caught it on its first execution.
- **(201) fields with no configured-flag make invented defaults
  API-invisible.** Radiative scalars carry no per-field set-state, so a
  serializer emitting every default reads back IDENTICALLY through the
  API. Only the written FILE shows the invention — such gates need a
  file-level leg, not more API assertions.
- **(202) ask the compiler for sizeof, never compute it.** Both hand
  computations of the struct pins were wrong (72 not 80; 40 not 48); a
  two-line probe got all three right at once.

### ⚠ Kdecay units TRIAGED and CONFIRMED — 86 400× plus unbooked annihilation (2026-08-31)

The debt H7b flagged and five handoffs carried. `[POLLUTANTS]` Kdecay is
1/day in the file (legacy `landuse.c:173` divides by SECperDAY at parse);
the new engine parses RAW and multiplies by dt-in-seconds at all five
application sites (QualitySolver nodes + link mixing, legacy binding, ARD,
LARD) with no conversion anywhere — every quality engine self-consistent,
all 86 400× too fast. Differential probe (KINWAVE, k=1/day): legacy loses
0.159 lbs of 134.7; the new engine outputs ZERO with a 100% continuity
error — `1 − k·dt = −29` clamps to 0, destroying the mass unbooked. Second
defect: the legacy-path QualitySolver never books decayed mass into
`qual_routing_reacted` at any k (ARD/LARD do). Record + KD1 fix handoff:
`KDECAY_UNITS_TRIAGE_2026-08-31.md`; evidence `tests/output/kdecay_triage/`.

- **(203) a self-A/B corpus is blind to a shared units error.** Five rounds
  of "corpus 21/21 byte-identical" were all true and all irrelevant: both
  sides of the A/B carried the same 86 400×. Only a differential against
  the REFERENCE engine sees the class — one such gate (KD1 §5.i) buys what
  no amount of self-comparison can.
- **(204) triage the oldest debt before it compounds.** The 100%-continuity
  annihilation was one 20-minute differential run away from being known,
  and it sat behind "un-triaged" for five handoffs while smaller items
  shipped. A flagged parity defect with a known reference costs less to
  triage than to carry.

### ✅ KD1 COMMITTED `3aa37c00` — Kdecay 1/day at every boundary, 1/sec inside; decay books its mass (2026-08-31)

Same day as the triage. Four defects in one round: the 86,400× units
error (parse now ÷SEC_PER_DAY, INP/GPKG/API convert back — the API's
1/day contract was already in its header); the unbooked legacy-path
decay (nodes, three link branches, reactions-active twin all book now);
every-node decay where legacy decays only storage-or-holding-volume
nodes; and mixAtNodes' evap factor firing on ANY volume decrease,
creating ~c·v_out per step at a draining storage (−47% measured; the
c_max cap masked it on every k=0 deck ever run). Cross-engine: Mass
Reacted 0.158 vs legacy 0.159 (k=1/day), 8.455 vs 8.523 (k=200/day);
184/184 ×3 (twice); corpus 21/21 re-earned after the mixAtNodes change.
Six falsifiers bite. Residual (BOTH engines, P2.4): transit mass takes
the decay factor outside volume-basis booking — legacy leaks 6.5–7% on
the same storage deck. Record: KDECAY_UNITS_TRIAGE §IMPLEMENTATION.

- **(205) when every layer's documentation agrees and the physics
  dissents, fix the physics.** The C header, the MCP docstring, the
  PollutantData comment and the file format all said 1/day; only the
  arithmetic said 1/sec. The store-1/sec design fell out of the existing
  gates: the CSTR benchmark and unit tests set the FIELD in 1/s, so
  storing 1/sec left every physics gate untouched and confined the
  change to boundaries.
- **(206) a bounds cap can hide a conservation bug for as long as the
  data stays uniform.** min(c_new, c_max) suppressed the evap-factor's
  mass creation on every uniform-concentration deck — which is every
  k=0 deck, which is the whole corpus. The first gradient (decay)
  exposed it at −47%. A cap that exists to bound a formula is a smell:
  ask what the formula does when the cap does NOT bind.
- **(207) a falsifier sweep needs its own falsifier: the restore.** The
  first sweep silenced build output and never verified restoration; a
  silently failed restore left F-i applied under F-ii…F-v, contaminating
  their bites — the "restored" tree still annihilated. Redone with
  cp-backups, build rc checks, and cmp-verification against reference
  snapshots after every restore.

### ✅ IO3c COMMITTED `183f59f3` — water age + ARD write their own config; IO3 save COMPLETE (2026-08-31)

Water age = the reactions pattern (swmm_water_age_save's body became the
shared transport::serializeWaterAgeConfig; %g upgraded to shortest-exact;
the seconds↔hours fixed point gated — gen2 == gen3 byte-identical). ARD
renders from the RAW rows; SCALAR_SCHEME/LIMITER ride provenance flags.
Found en route: the IO3a RENDER path silently replaced different
pre-existing configs where the copy path announces — now both announce.
184/184 ×3; corpus 21/21; five falsifiers bite. Every registered
component now saves its own config — the embedded-section mitigation
(7d43a1ff) is superseded by construction for external configs.

- **(208) an alias whose canonical carrier is conditionally written needs
  provenance, not deletion.** SCALAR_SCHEME aliases FV_SCALAR_SCHEME, but
  the INP writer emits FV_* only under FLOW_ROUTING FV — on a DYNWAVE
  deck the component file is the ONLY carrier. Dropping the alias (first
  design) lost state; rendering it unconditionally invented state. The
  apply hook records who spelled it; the renderer re-emits at the live
  value exactly then.
- **(209) a replacement code path inherits every contract of the path it
  replaces — enumerate the old path's gates before switching.** The
  render path bypassed the copy path's announce-on-replace warning; gate
  7 (written for the copy path) caught it in the full census after the
  targeted suites had all passed. Run the census before celebrating.

### ✅ L3 COMMITTED `ec22580a` — MSX species ride LARD segments and react; the last unstarted quality step (2026-08-31)

Two scoping facts reshaped the estimate downward: the store was already
row-generic (H7a/H7b's layout work paid off — L3a was "append the species
table to rowLayout() and give MIX a third reserved-row class"), and the
reference bar was ARD alone (LEGACY never transported MSX either).
ensureMsxState promoted from the binding's anon namespace (R4b warning
now LEGACY-scoped, where it is still true); reactSpeciesBlock wraps the
shared integrator with the reactElements contract. Deferral warning
deleted. Cross-engine ratio 0.9751 (segments vs cells, same deck).
184/184 ×3; corpus 21/21. Record: L3 handoff + falsifiers.log.

- **(210) a tolerance band is calibrated by its falsifier, not by taste.**
  The off-by-np gather bug read cross-engine ratio 1.1461 — inside the
  0.35 band by miles and inside 0.15 by 0.004. The band that ships is
  0.10: the honest answer (0.9751) clears it 2.5x over, and the one bug
  the gate exists to catch cannot. Run the falsifier BEFORE choosing the
  band, and set the band from the gap between honest and broken.
- **(211) "not a new module, just a binding" was half true, and the half
  that was false was cheap ONLY because two earlier rounds paid for it.**
  H7a's explicit SpeciesRowLayout and H7b's add-a-row precedent are what
  made species rows a mechanical extension. Layout debt compounds;
  layout INVESTMENT compounds too.

### ✅ H6b COMMITTED `89310068` — bed conduction, deep-ground conduction, hyporheic exchange (2026-08-31 → 09-01)

[HEAT_FLUXES] SEDIMENT_EXCHANGE stops refusing itself: per-link bed
temperature + per-species bed concentration, wetted-perimeter conduction
and hyporheic advection to the channel, conduction to a fixed or
timeseries deep-ground boundary. Coupled water/bed pair stepped by the
exact 2×2 matrix exponential (real non-positive eigenvalues: no
oscillation, no overshoot at any dt); solutes in (total, difference)
coordinates so conservation is structural. LEGACY link store only —
ARD/LARD decline by name (mapping a per-link bed onto cells/parcels is a
modelling decision, recorded on the array a successor would widen).
Validated on the LANDING base fd9f6b94 after six peer commits moved it
mid-round: 185/185 ×3 clean, corpus 21/21, seven falsifiers bite,
[SEDIMENT_EXCHANGE] survives swmm_model_write gen2==gen3 byte-identical.
Reference: HydroCouple HTSComponent; three recorded divergences
(wetted perimeter not top width; v_hyp velocity not discharge; streambed
1670/1807 not bioretention 1970/2758).

- **(212) a written cost prediction is a load-bearing artifact — when it
  is falsified, hunt down every copy.** "H6b is one term in netFluxOut"
  lived in the plan AND in HeatFluxes.hpp, and both would have taught the
  next flux-family author a wrong cost in the expensive direction. The
  bed acts on a different area (nonzero exactly when the surface area is
  zero) and adds a second state variable — either alone breaks the
  prediction. Both copies now carry the correction and the reason.
- **(213) retiring a deferral flips the gates that pinned it — grep for
  the refusal message before running the census.** Two tests
  (heat_surface, heat_radiative) pinned "SEDIMENT_EXCHANGE must refuse"
  and failed ctest pass 1; the surface gate's own comment states the
  rule, written when H3 performed the same flip one module over. The
  handoff's file list missed both, plus the test registration (the
  3rd-time trap, 4th time) and its own §2 header edit.
- **(214) a falsifier that bites HARDER than predicted is still a pass —
  record the miss.** Forward-Euler-izing relaxPair was predicted to fail
  gates 3+9 with 1 passing; it failed 1, 3, 4, 9 (Euler does not reduce
  to relaxT's exponential under EXPECT_DOUBLE_EQ). The core claim — the
  invariant gates 2/6/8 cannot see a wrong integrator — held exactly.
  Same for falsifier iv: "surcharged goes flat" measured as an 8×
  collapse with a 0.85 mK residual, and the residual's cause (the
  pre-surcharge filling transient) is itself confirmation.

### ✅ Debt batch COMMITTED `6264eb8a` + `d868b2c3` + `3e87868e` — ARD relaxes, ts-name getters, save idempotence (2026-09-01)

Three of H6b's §10 debts, one worktree cycle, validated together
(185/185 ×3 clean; corpus 21/21 — inert by construction on all three)
and committed separably. Evidence: tests/output/ard_relax_batch/.

- **(215) a discriminating deck is engineered, not assumed — and the
  first regime you reach for may be a different defect's.** The draining
  thin-film deck built to expose forward Euler exposed the ADVECTION
  stage instead (6e+117 degC in cell state with the integrator already
  fixed, CFL-starved at a forced 300 s step) — and windless decks never
  bite at all (J' too shallow), and Euler's downward explosions hide in
  the transport's max(0, mass) clamp as a LITERAL 0 degC. The gate that
  ships is a STEADY warm sheet: hot air over a cold 1 mm trickle, where
  transport is quiescent, the clamp is on the far side, and base reads
  an impossible 0 degC while patched reads 7.1/10.8/30.0.
- **(216) new pre-existing defects found while engineering the gate,
  recorded not fixed:** ARD advection unbounded under CFL starvation on
  draining decks; temperature rides the non-negativity clamp (a 0 degC
  floor — wrong for a scalar whose zero is not special); the temp-row
  publish quotient below legacy's ZeroVolume (THIS one fixed here: hold
  the last reading, H1's carried-temperature convention).
- **(217) the leap-year anchor asymmetry: parse and format must share a
  calendar.** SWEEP dates parsed against 2000 (leap) and formatted
  against 2001 (non-leap) — every date past Feb 28 walked one day per
  save cycle, including the untouched DEFAULT. One anchor now (2001),
  clamped for pre-fix stored 366s.
