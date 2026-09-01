# Unified Transport Program — comprehensive state review (2026-08-25)

**Method:** every claim below was verified **against the source tree**, not
against the plan documents. Where a document and the code disagree, the code
wins and the disagreement is reported. Three parallel audits: the plan
enumeration, the expedited X/Y/Z track, and Phases 2–5 + GUI.

**Headline: the program is substantially further along than `PROGRESS.md`
says, and its bookkeeping is wrong in both directions.** Phase 5 (LARD) is
mostly delivered while the tracker says "not started, 0 of 4". Several Phase 1
rows marked ⬜ are in fact delivered under different labels. And two phases
that the tracker's phase table treats as ordinary future work are not merely
unstarted — one of them is *actively refused* by the code.

---

## 1. Corrected phase table

| Phase | `PROGRESS.md` says | **Verified** |
|---|---|---|
| 1 — 1D transport | ACTIVE, 34 of ~45 | **ACTIVE, ~40 of ~45** — see §2; several ⬜ rows are delivered under other labels |
| 2 — HydroCouple | ⬜ 0 of 4 | **0 of 4, and HC2 is a hard rejection path, not a gap** (§3) |
| 3 — 2D transport | ⬜ 0 of 7 | **0 of 7, genuinely zero code** (§4) |
| 4 — Groundwater | 🔄 sign-off, 0 of 4 | **0 of 4 code; G0 sign-off still owed** (§5) |
| 5 — LARD | ⬜ **0 of 4** | **❌ WRONG — L0, L1–L2, L4 DELIVERED and validated; L3, L5–L7 open** (§6) |
| GUI | ⬜ 0 of 14 | **❌ WRONG — G1g, G2g, G3g implemented; G5g partial** (§7) |

**`PROGRESS.md` is stale by roughly two weeks of parallel work.** It is
written from this session's line of sight and does not know about the X, Y, Z
and closeout tracks that landed 2026-08-23 → 08-25 in other sessions.

---

## 2. Phase 1 — corrections to the roadmap's own status marks

Everything the roadmap marks ✅ in the E, R, IO and H tracks **verified as
genuinely implemented with tests**, with these exceptions and corrections:

### 2.1 Marked ⬜ but actually delivered (the roadmap under-reports)

| row | reality |
|---|---|
| **R5** (reaction APIs) | **Mostly EXISTS.** `swmm_reaction_validate_expression` is real — `openswmm_reactions.h:78`, impl `openswmm_reactions_impl.cpp:114` — inside a ~38-entry-point authoring surface (804 LOC impl, `test_reactions_api.cpp` 555 lines). It shipped under labels **E-C1/E-C3**, not R5. **Only the Python and MCP halves are missing.** |
| **IO5** (component APIs) | **PARTIAL, not absent.** `swmm_process_component_count/get/find/register/remove` at `openswmm_process_components.h:46-72`. Missing precisely: reload/staleness, Python, MCP. |
| **A5** (LARD age binding) | **Delivered** — `9f155227`. The roadmap now records it; `PROGRESS.md` does not. |

### 2.2 Marked ✅ but only half true

| row | reality |
|---|---|
| **IO3** | **PARTIAL — and this settles the roadmap's internal contradiction.** The `[PROCESS_COMPONENTS]` pointer section and carry-alongside file copy DO exist (`InpWriter.cpp:2520-2569`). Per-component `saveData()` does **not**, and the code says so at `:2574`, then **warns the user that embedded `[REACTION_*]` sections are lost from the save** (`:2580-2586`). A real serializer exists (`ReactionsWriter.cpp`, 183 LOC) but is wired only to the C API, never to `InpWriter`. **Embedded-section round-trip is genuinely broken.** |
| **H4** | Delivered, but its **G-UT3 analytical gate was never delivered** — recorded in the roadmap, absent from `PROGRESS.md`. |

### 2.3 Confirmed genuinely open

E2b (all three parts; tidal reverse-flow BC has *no* scaffolding at all), E6
(`openswmm_transport.h` does not exist), R4b (LEGACY MSX transport — the code
warns "arrives with R4b" at `ReactionLegacyBinding.cpp:96`), A2c, IO4, H6,
H7.

### 2.4 H6 is not uniformly absent

`shade_factor` exists and is honoured (`RadiativeExchange.cpp:65-69`,
`SHADE_FACTOR` parsed at `HeatComponent.cpp:138`) — but as a **static user
constant**, not computed shade. No solar-position module, no sediment layer;
the latter is a documented deliberate omission (`RadiativeExchange.hpp:53-59`).

---

## 3. Phase 2 — HydroCouple is refused, not merely unstarted

This is the finding most likely to change planning.

- **HC1 ABSENT.** No `HydroCouple*.h` anywhere; no `find_package`. The
  `/Users/calebbuahin/Documents/Projects/HydroCouple` checkout **is not
  referenced by the engine build in any form.** The only build-file hits are
  branding strings.
- **HC2 ABSENT and actively rejecting.** `PluginType` has exactly four
  enumerators — `INPUT, OUTPUT, REPORT, STATE_IO`
  (`IPluginComponentInfo.hpp:75`); no `PROCESS_COMPONENT`.
  `hydrocouple_component_info` exists **only in two prose comments.** The
  factory's sole `dlsym` target is `openswmm_plugin_info`
  (`PluginFactory.cpp:253`). And there is a **rejection path**: a
  `[PROCESS_COMPONENTS]` row naming a shared library is detected
  (`ProcessComponentRegistry.cpp:62-73`) and **hard-errors** with "not
  available yet (arrives with plan phase HC2)" (`:216-223`).
- **HC3/E7 ABSENT.** `IModelComponent` occurs **once in the whole repo — in
  `paper/paper_v2.md`.** Zero source occurrences.

**What this means:** the entire `[PROCESS_COMPONENTS]` machinery is an
**in-process string-id registry** with HydroCouple-*flavoured* naming
(`org.hydrocouple.openswmm.*`). No ABI, no dynamic loading, no HydroCouple
types. The naming makes it read as further along than it is — including in
our own documents.

---

## 4. Phase 3 — zero code, and a trap for grep-level audits

**S1–S7 all ABSENT.** `src/engine/2d/` (48 files) contains no scalar transport
whatsoever: no `SurfaceTransportState` (0 hits repo-wide), no species arrays in
`SurfaceStateData.hpp`, no species tuple in `NodeCoupling`, no
`openswmm_transport2d.h`, no 2D transport INP sections, no species variables
in the 2D output plugin.

**⚠ The trap:** "advection" *does* appear throughout `src/engine/2d/` — it is
**momentum** advection in the local-inertial scheme (`SolverOptions2D.hpp:216`,
`InertialKernels.hpp:242`, `ExplicitInertialSolver.cpp:449-454`). A keyword
audit would score S1 as landed. It is not.

`org.hydrocouple.openswmm.integrated2d` exists only as a registry row that
errors with "arrives with plan phase S1/G1".

---

## 5. Phase 4 — zero code, and a second trap

**G1 and G2 have no code.** No `openswmm_gw2d.h`, no σ-column, no closure A/B,
no explicit-LTS GW scheduler. The only named artefacts are **reserved enum
placeholders** — `SUBCATCH_AQUIFER`/`AQUIFER_2D` at `Infil2D.hpp:85-86` — which
parse and serialize but which **the C API refuses** at
`ApiInfil2D.cpp:124-130`.

**⚠ Two disambiguations that matter:**

1. **Track I (per-cell 2D infiltration) IS delivered** and is a *different
   track* from G1/G2 — but `Infil2D.hpp:21` cites the two-zone groundwater
   plan as its parent document. **A doc-driven audit scores G1 as landed
   because of that one citation line.**
2. **LTS in the 2D tree is the surface marcher's tiering**, unrelated to the
   GW explicit-LTS scheduler.

**G0 sign-off (D-N1–N5) remains owed since 2026-08-15** — ten days, and it
gates the whole phase.

---

## 6. Phase 5 — LARD is largely DELIVERED (the tracker is simply wrong)

This is the expedited work. All eight commits verified present on
`swmm6_rel`, subjects matching, and **every hunk-presence grep the handoffs
prescribe reproduces in the tree today.**

| step | status | evidence |
|---|---|---|
| **L0** wiring | ✅ `24602eb2` | dispatch live at `SWMMEngine.cpp:3643-3651` |
| **L1–L2** segment store + LTD advection | ✅ `8c141a5e` | `SegmentStore.hpp` 288 lines; `LagrangianSolver.hpp` 576 lines |
| **L3** MSX reactions on segments | ⬜ | only first-order KDECAY reacts; warning live at `SWMMEngine.cpp:350` |
| **L4** RWPT dispersion | ✅ `647a3603` + `b9852cee` | `RwptDispersion.hpp` 356 lines, splitmix64, D-X3b1 limiter |
| **L5–L7** age / APIs / gates / perf | ◐ | age ✅ `9f155227`; C API ✅ `d7b6c079`; Python+MCP ⬜; H7 ⬜ |

Plus **X6** negative sources (`d79c8bcf`, all three engines, and it found a
**pre-existing ARD silent ledger break** dating to E1), **Y0** transport
options C API (`948b2840`), **Z1** age as an inflow constituent (`4639be37`),
**H1** C-API numeric hardening (`d80bba34`), and GUI rounds Y2b-1/2/3, Y4,
Y3b. Test volume: **2,213 lines** of LARD suites; corpus now **19 decks**.

**Amendment 1 (2026-08-23)** reverses the earlier "age is not a fake
pollutant" line **for water age only** — age becomes a first-class species in
the UI and gains a real inflow pathway, *without* becoming a `[POLLUTANTS]`
row. That distinction is load-bearing: it preserves the np/nr stride
separation three landed rounds depend on.

### 6.1 What LARD still owes

P1.4 `[TRANSPORT_SOURCES]` negative rows (unspecified behaviour today);
P1.5 negative DWF/GW/RDII (plan recommends closing as "won't do"); P2.1–P2.5
(heat under LARD, MSX reactions on segments, treatment interop, storage mixing
beyond CMSTR, full A6 Python+MCP); P3 (laminar RWPT deck, **an RWPT corpus
deck — none exists**, `swmmvis_core` extraction). Plus debts the closeout does
not list but the handoffs do: the LARD mix's **deliberately omitted evaporation
up-concentration and `c_max` clamp**, per-(element,species) clamp warnings, A2c,
age hotstart SAVE fidelity, L6 perf pass, divider mass-split.

---

## 7. GUI track — three phases implemented, not zero

| phase | status | evidence |
|---|---|---|
| **G1g** options page | ✅ implemented | `simulationoptionsdialog.cpp:1173` — solver combo, LARD group, reserved-species checkboxes |
| **G2g** reaction editor | ✅ implemented, substantial | `reactionsystemeditordialog.cpp`, **1150 lines**, 7 tabs |
| **G3g** water age editor | ✅ implemented **and reachable by two paths** | `wateragesourcesdialog.cpp`; the "Y3b owed / unreachable" note is **stale** |
| **G5g** result descriptors | ◐ partial | `resultdescriptor.h`, `speciesattributes.h`; per-species units from `.out` still owed |
| **G4g** heat editor | ⬜ absent | checkbox only; no heat dialog file exists |
| **G6g** 2D transport rendering | ⬜ absent | `ScalarFillSublayer` 0 hits |
| **G7g** property edits | ⬜ absent | 0 hits for the named properties |

**⚠ `Mesh2DGroundwaterDialog`** (245 lines) is a **display-only preview** whose
own banner says nothing is written to the model — "the single artefact most
likely to be miscounted as GW work landed."

---

## 8. Bookkeeping defects found in the plan documents themselves

These are why the state was unclear, and they are worth fixing before the
next planning decision.

1. **`PROGRESS.md` is stale by two weeks** on Phases 5 and GUI, and its phase
   table is actively misleading (LARD "0 of 4").
2. **The X, Y, Z and closeout tracks are not in the roadmap's canonical step
   tables at all** — `grep X4|Y0|Z1` in `IMPLEMENTATION_ROADMAP.md` returns
   **zero hits**. They exist only in the subplan and the individual handoffs.
3. **Roadmap self-contradiction on IO3 and IO5** — settled in §2.2/§2.1: both
   are PARTIAL.
4. **R5 mislabeled** — delivered under E-C1/E-C3, still marked ⬜.
5. **Two live label collisions.** Phase 3's **S1–S7** vs the snow track's
   **S1–S4** (five shared labels in one document); and **Y4** means two
   different rounds in the same document family.
6. **In-source doc drift** — `ArdEngine.hpp:45-47` still says treatment,
   sources/BCs and ledger rows are "pending (E5)", all three landed;
   `SimulationOptions.hpp:88-91` still says LARD is "skeleton dispatch only,
   transport lands in X2", five rounds after X2 landed. **Header docblocks are
   currently a *less* reliable source than the plan docs.**
7. **X5 §10 declares "THE ENGINE TRACK IS COMPLETE"** — false as written; Z1
   and H1 are engine commits that landed after it.
8. **The closeout plan is stale on P0.1** (push) — that is done; the tree is
   `[ahead 3]` on post-LARD commits.

---

## 9. What I would actually do next, in order

1. **Reconcile the trackers** — one session, no code. Fold X/Y/Z/closeout into
   the roadmap's canonical tables, fix IO3/IO5/R5/A5, rename the S-collision
   (`SN1…SN4` for snow, as the roadmap itself suggests), rewrite
   `PROGRESS.md`'s phase table from §1 above. **Every planning decision from
   here is being made on numbers we now know are wrong.**
2. **Close the quality-ledger units round** already in validation — it is the
   last of the hydrology/reporting cluster.
3. **G0 sign-off** (D-N1–N5). Ten days owed, pure review, and it gates all of
   Phase 4.
4. **Then choose the next real track**, with the corrected picture:
   - **H6/H7** finishes heat (H7 needs `openswmm_heat.h`, which does not exist);
   - **L3 + P2.3** finishes LARD's chemistry;
   - **E6/A6/R5-Python** finishes the API surface — largest user-visible gain,
     and R5's C half is already done;
   - **HC1–HC3** is a bigger commitment than the tracker implies (§3);
   - **Phase 3/4** should not start before G0 sign-off and a decision about
     whether HydroCouple is still the integration path.

---

## 10. The one-line answer

**Phase 1 is ~40 of ~45 and effectively complete for the ARD/reactions/age/
heat core; Phase 5 is most of the way done; the GUI has three real editors;
and Phases 2, 3 and 4 have no code at all — with Phase 2 in a state of
explicit refusal rather than absence.** The program's largest current risk is
not any of the open steps: it is that **three separate trackers disagree with
the code and with each other**, and the last several planning decisions were
taken against the wrong numbers.
