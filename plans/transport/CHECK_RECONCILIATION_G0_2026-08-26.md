# Check report — reconciliation + G0 sign-off round (2026-08-26)

**Checking:** `RECONCILIATION_AND_G0_HANDOFF_2026-08-25.md`.
**Tree:** engine HEAD `a0dbc04b`, **46 commits past** the handoff's stated base
(`55a70839`), with **334 modified files** from concurrent sessions.

**Protocol deviation, declared up front.** §3 says to answer the fourteen
questions before reading §4. I read the handoff top-to-bottom before starting,
so *I* am contaminated. Independence was restored by delegating all fourteen
questions to four subagents that were given §3's wording only, were forbidden
to read anything under `plans/`, and never saw §4. Every "re-derived" answer
below is theirs; the diffing is mine.

**Verdict: the round is sound and lands. Nine of fourteen claims confirmed
exactly. Five need correction, two of them materially — and one of those
undermines a decision the round used to close G0.** Separately, the sign-off's
*authorship* needs your confirmation (§6).

---

## 1. Build check (§2)

| check | result |
|---|---|
| `cmake --build build/darwin` | **clean**, exit 0 |
| `ctest -j8` ×3 | run 1: 176/177; runs 2-3: **177/177** |
| the one failure | `test_engine_report_timing` — **passes standalone**, and green in two of three parallel runs. Intermittent `-j` collision (the shared-`data/`-cwd family), not a regression, not this round |
| corpus | **not run as base-vs-patched — replaced with a stronger proof, see below** |

Note the *expected* pre-existing failure named in the house protocol,
`test_engine_2d_infil_integration`, is **now green**: peer commit `002b8c0a`
fixed it. The engine suite currently has no standing failure.

**The corpus run was skipped deliberately, and replaced with something
stricter.** `run_corpus.sh` diffs two binaries; for a comment-only round the
two binaries are the same program, so 19/19 identical would be a tautology
dressed as evidence — and in *this* tree it would also be unattributable,
because 334 peer-modified files sit between HEAD and the working state.
Instead both files were comment-stripped and whitespace-normalized and
compared against `HEAD`:

```
COMMENT-ONLY  src/engine/transport/components/EulerianArdComponent/ArdEngine.hpp
COMMENT-ONLY  src/engine/core/SimulationOptions.hpp
CODE CHANGED  src/engine/core/InpWriter.cpp     ← positive control (a peer's work)
```

The compiler cannot observe this round's change. "Must not move a single
test" is therefore established structurally rather than empirically, which is
the stronger claim. (Tool:
`scratchpad/strip_comments.py`, with the positive control shown above so the
negative result is credible.)

**Attribution note for §7.** The quality-ledger units fix is no longer loose:
all five of its files (`SWMMEngine.cpp`, `openswmm_massbalance_impl.cpp`,
`DefaultReportPlugin.cpp`, `legacy/engine/landuse.c`, `test_massbalance.cpp`)
are clean/committed. That round has landed; §7's collision hazard has expired.

---

## 2. The fourteen claims

**Confirmed exactly (9):** 3, 4, 5, 6, 7, 9, 11, 12, 14.

Highlights of the confirmations, since a confirmation with new evidence is
worth more than a tick: claim 11's sizes reconcile *commit by commit*
(`SegmentStore.hpp` 270 + 12 + 6 = 288 exactly), and the LARD code is live,
not orphaned — `SWMMEngine.hpp:392` holds the member, `SWMMEngine.cpp:3653`
calls `lard_.step(...)`. Claim 12's age editor is reachable by **two** paths
(Model-menu action `actionEditWaterAgeSources` → `swmmvis.cpp:6825`, and an
"Edit Source Ages…" button at `simulationoptionsdialog.cpp:1290-1293`), so the
"Y3b owed / unreachable" note is confirmed stale. Claim 4's absence is airtight:
**`G-UT3` appears nowhere in the tree**; H4 is gated only against LEGACY
(`EXPECT_NEAR(leg[i], ard[i], 2.0)`), and `erfc`/`std::erf` appear **zero**
times repo-wide, so no closed-form reference exists anywhere.

### Corrections

| # | claimed | measured | severity |
|---|---|---|---|
| 1 | "~38 entry points" | **33** (declared set diffed against defined set; 33 == 33) | minor — the claim carried a `~` |
| 2 | ":2580-2586 **warns the user** embedded sections are lost" | the warning is **unreachable in production** — see §3 | **major** |
| 8 | "**Once**, in `paper/paper_v2.md`" | **38 occurrences across 11 files** (37 under `plans/`, 1 in the paper). The *substance* — zero source occurrences — is correct | method (see §5) |
| 10 | "**No** GW kernel" | a **two-zone groundwater model does exist** — `Groundwater.hpp:19-30`, class `GWSolver`, RKF45, legacy `gwater.c` parity. What is absent is the σ-column / 2D / LTS kernel | wording |
| 13 | 4 fixed things | **11** — see §4 | **major** |

Claim 7 confirms, but is narrower than "hard error": the rejection is hard
only on a **strict** open. Under `lenient_open_` (the editor path) the row is
recorded and **skipped**, the open continues — and because the spec is stored
before rejection, `InpWriter.cpp:2520-2521` **round-trips the `.dylib` row
back out** on a subsequent save. There is **no test coverage** for any of it
(`grep "library-loaded\|looks_like_library" tests/` → 0).

---

## 3. ⚠ Embedded `[REACTION_*]` are lost SILENTLY, not with a warning

Claim 2 says the engine "warns the user embedded sections are lost". The
warning exists in the source, and in production **nothing ever receives it**.

`writeInpFile`'s warnings sink is an optional out-parameter defaulted to null
(`InpWriter.hpp:75-77`), and the warning is gated on it. **Every production
call site passes nothing:**

- `openswmm_model_impl.cpp:265` — `swmm_model_write`
- `openswmm_model_impl.cpp:276` — `swmm_model_write_with_plugin`
- `DefaultInputPlugin.cpp:203` — `DefaultInputPlugin::write`

Only *tests* pass a sink, including the test that certifies the behaviour
(`test_reactions_config.cpp:294`, `EmbeddedSectionsLostOnSaveAreReported`) —
which calls `writeInpFile` directly and therefore **cannot detect this gap**,
while its own body confirms the data loss. No C entry point surfaces writer
warnings at all.

**This reaches the GUI.** `swmmvisprojectwindow.cpp:1432` saves through
`swmm_model_write_with_plugin`, and the GUI has no handling for this message
(`grep "embedded component\|lost from this save"` over the GUI → nothing). So
a user who opens a deck with embedded `[REACTION_*]`, edits anything, and
saves, **loses their reaction system with no message anywhere**.

The G0 recommendation calls IO3's round-trip "broken in a way users can hit".
It is worse than that: it is broken **silently**, which is the failure mode
this program's own E1-era rule exists to forbid.

---

## 4. ⚠ D-N1's "chore" re-rating is not supported

G0 re-rated D-N1 from *risk* to *chore* on a four-item list. The re-derived
list is **eleven**, and the two most dangerous are on the GPU path the
original list touched only glancingly:

*Confirmed as claimed:* tier lists are already
`std::vector<std::vector<int>>` (`ExplicitInertialSolver.hpp:150-151`);
`std::array<long,8> tier_occupancy_` (`:205`); CPU clamp
(`ExplicitInertialSolver.cpp:79`); Kokkos clamp (`ExplicitKokkosSurfaceSolver.cpp:232`,
not `:230`); parser bound (`SectionHandlers2D.cpp:189-193`).

*Missed, and load-bearing:*

1. **`ExplicitKokkosSurfaceSolver.hpp:137-138` — `std::array<int,9> tier_off_`
   and `ftier_off_`.** K+1 segment offsets; `tier_off_[K]` with K==8 already
   writes the last valid slot. **Raising K to 9 writes index 9: out of
   bounds.** This overruns *first*.
2. **`ExplicitKokkosSurfaceSolver.hpp:149` — `std::array<long,8>
   tier_occupancy_`, indexed unguarded** (`:589`, `:1325`), with
   `s.n_tiers = K_` (`:1323`) set with no `min` against the array size →
   **buffer overflow**, where the serial path merely truncates.
3. `ISurfaceSolver.hpp:116` — `long tier_cells[8]` in the solver-agnostic
   telemetry ABI, plus `n_tiers` documented "≤ 8".
4. `SimulationContext.hpp:1276` — `long solver_tier_cells[8]`.
5. `SurfaceRouter2D.cpp:1091` — `for (k = 0; k < s.n_tiers && k < 8; ++k)`,
   a hard literal-8 copy guard.
6. Docs stating the ceiling (`Chapter9-TwoDimensional.md:577`, `:1345`).

So D-N1 is not "one telemetry array, two clamps, a parser bound". It is a
**cross-cutting ABI change** spanning the serial solver, the Kokkos solver,
the solver-agnostic telemetry struct, the mass-balance context and the router,
with **two silent memory-safety failures** on the GPU path if the clamp is
raised without them. That is a risk, not a chore — which is what the
recommendation document itself predicted would happen if anyone re-checked
("a read of one site is not a reading of the chain"). It was right.

**Recommend: re-open D-N1's rating** and re-scope before anyone budgets it.

*Adjacent, found in the same sweep:* `GeoPackageReader.cpp:198` reads
`2D_LTS_TIERS` with **`std::stoi` and no range check**, while the INP and C-API
paths both enforce 1..8. A `.gpkg` carrying `2D_LTS_TIERS 40` loads, and is
silently rescued later by the solver's clamp.

---

## 5. ⚠ A methodological finding that affects other counts

Claim 8's "once" is not a mis-read — it is a **tooling artifact**, and it will
recur. The shell's `grep`/`find` are wrapper **functions** that silently skip
`.gitignore`d paths. In this repo `/plans/*` is gitignored, so *every plan
document is invisible* to an ordinary `grep .`. That turns 38 hits into 1 —
exactly the claimed number.

**Any count in the audit derived from a plain `grep` under-reports by exactly
the plans/ corpus.** Re-derive with `/usr/bin/grep` when documents are in
scope. (The subagent caught this only because its first pass returned 1 and it
distrusted the number.)

---

## 6. ⚠ Who signed off G0?

Two documents, same date, in direct conflict:

- `G0_SIGNOFF_RECOMMENDATION_2026-08-25.md` is framed "**This is a decision
  document, not a changeset**", addressed to you, and its §6 — *"What I need
  from you"* — asks **four questions**, closing: "Answer those and G0 closes."
  It was never updated.
- `TWO_ZONE_GROUNDWATER_..._PLAN.md:906` records
  "**✅ G0 SIGN-OFF — CLOSED 2026-08-25 (C. Buahin)**", and
  `IMPLEMENTATION_ROADMAP.md:2579` says "**G0 SIGNED OFF 2026-08-25 (user)**",
  with every decision recorded **verbatim as the recommendation recommended**,
  including D-N2's "deferred to step 18" nuance.

I cannot tell from the tree whether you answered those four questions. What I
can say: nothing anywhere records answers distinct from the recommendations;
the recommendation still asks; and the convention used for decisions you
demonstrably *did* make is different and explicit
(`plan:883`, "recorded 2026-08-20, **user-approved**").

**This needs your one-line confirmation.** If you did not answer, a decision
gate for a large new subsystem (G1, now marked "ready to start") is recorded
in three documents under your name, and D-N1's re-rating — which §4 above
shows is wrong — is part of it.

---

## 7. Falsifiers (§5)

| # | expected | result |
|---|---|---|
| i | X4/Y0/Z1 hit the roadmap | **PASS** — 9 hits |
| ii | no bare `S1-S7` rows; snow labels untouched | **PASS** — 0 bare rows, 9 `2D-S` rows; `S1/S2a/S2b/S3/S4` intact across **17** handoffs; `2D-S` confined to this round's four documents |
| iii | both repaired docblocks describe current behaviour | **FAIL — see below** |
| iv | are the G0 conditions binding? | **decorative in effect** — see below |
| v | counts still lower bounds; close the gap | **CLOSED — see §8** |

**iii fails.** The repaired `SimulationOptions.hpp:85-100` says the NOT-YET
list is "**each** behind a live open() warning". Three of the four are
(`SWMMEngine.cpp`: heat/H7, reactions/L3, treatment). The fourth — **storage
mixing beyond CMSTR — has no warning anywhere**; `grep CMSTR` finds only the
comment itself and `LagrangianSolver.hpp:79`, whose own wording marks
treatment as the "(warned bypass)" and pointedly does not so mark storage
mixing. There is also no `FIFO`/`LIFO`/`WEIGHTED` vocabulary in the non-legacy
engine at all, so arguably there is nothing yet to warn *about* — in which
case the honest comment is "no input surface yet", not "warned".

A round whose purpose was repairing comments that overclaimed has left a
comment that overclaims. Small, but it is the exact disease.

**Applied** (the only change this check made to source): the NOT-YET list now
names the three items that *are* warned and says storage mixing is absent but
unwarned because no input surface exists for it. Re-verified comment-only by
the same stripper, and rebuilt clean — the round stays comment-only.

**iv: recorded consistently, enforced nowhere.** Both conditions appear in all
three documents, and the plan recorded the *stricter* branch of the
recommendation's "add a 2D deck **or** amend §11" disjunction — good. But
neither is attached to an executable artifact: nothing in `tests/`, CI or the
corpus runner fails if D-N1 lands without a 2D deck, and nothing fails if ET
retirement merges without a migration guide. A future round satisfies the
letter by not looking.

*Cheap fix, and the parts already exist:* `tests/scripts/trackI_bitwise_regression.sh`
**is** a bitwise 2D gate — two binaries, byte-compared `.out`, `.rpt` with the
banner stripped — and it discovers **32 qualifying 2D decks** dynamically. It
is simply not wired into `run_corpus.sh`. Wiring it in, or adding a test that
asserts `MANIFEST` contains at least one 2D deck (it fails today, so it lands
with D-N1's first commit), converts the condition from prose into something
that bites.

---

## 8. Counts — the 2026-08-22 gap is now closed (falsifier v)

`PROGRESS.md:46` asks for exactly this and says nobody has done it. Measured
against `build/darwin` at `a0dbc04b`:

| quantity | measured |
|---|---|
| registered ctest tests | **177** (175 `unit`, 1 `regression`, 1 unlabelled) |
| gtest gates, tree-wide | **2,786** across **317** test source files |
| test LOC, tree-wide | **296,677** |
| gates in test files added since 2026-06-01 | **976** across **119** files |
| test LOC, same window | **48,400** |
| parity corpus decks | **19** (0 are 2D) |

`PROGRESS.md`'s standing figure — "216+ test gates across 25+ new test files"
— is low by **4.5×** on gates and **4.8×** on files. It was honestly flagged as
a lower bound; it can now be replaced with a measurement.

*Doc drift found in passing:* `tests/parity/README.md` headlines 15 decks and
its table sums to 18, while `MANIFEST` holds 19; `MANIFEST:51`'s own section
header says "Water age and heat (3)" over 4 decks.

---

## 9. §8's prepared commit will not do what it says

`plans/` is **gitignored by explicit decision** (`.gitignore:9-16`, dated
2026-08-21 in the file's own comment: "everything else under `/plans/` stays
out of the tree"), and **zero files under `plans/` are tracked** — including
`SNOW_DIVERGENCE_REGISTER.md`, which the ignore file deliberately re-includes
but which was never added.

So the prepared commit — whose message is entirely about reconciling trackers,
folding in Phase 1x, relabelling Phase 3 and closing G0 — would land **two
comment-only docblock edits and nothing else**. Anyone reading it in history
sees a message describing documents the repository does not contain.

Not a defect in the work; a mismatch between the message and what git will
accept. Either narrow the message to the two docblock repairs (and reference
the plan documents as out-of-tree), or raise the tracking question separately.
Standing house rule is that workplans are not committed, so the message should
change, not the ignore file.

---

## 10. Recommended actions, ranked

1. **Confirm or deny the G0 sign-off authorship** (§6). Everything in Phase 4
   sequencing rests on it.
2. **Re-open D-N1's risk rating** and re-scope against the eleven-site list;
   in particular the Kokkos `std::array<int,9> tier_off_` overrun and the
   unguarded occupancy array (§4).
3. **Fix the silent save-as data loss** (§3) — the smallest honest fix is
   routing writer warnings into `ctx.warnings`, the channel the read side
   already uses and the GUI already surfaces.
4. **Correct the `SimulationOptions.hpp` NOT-YET list** (falsifier iii) —
   storage mixing has no warning.
5. **Amend claim 1 to 33 entry points, claim 8 to 38/11 files, claim 10's
   wording** ("no σ-column/2D/LTS GW kernel" — a two-zone GW model exists),
   and claim 7 ("hard error on strict open; skipped and round-tripped on
   lenient open, untested").
6. **Replace `PROGRESS.md`'s lower-bound paragraph** with §8's measurements.
7. **Rewrite §8's commit message** to match what git will actually take (§9).
8. Cheap and worth doing while it is in view: bound `2D_LTS_TIERS` in
   `GeoPackageReader.cpp:198`, and wire `trackI_bitwise_regression.sh` into
   the corpus so D-N1's condition can bite.
