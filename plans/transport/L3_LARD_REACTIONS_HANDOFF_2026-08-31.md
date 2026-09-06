# L3 — MSX species ride LARD segments and react — Handoff (2026-08-31)

**For:** the implementing/checking agent (same session, IO3c's successor).
**Base:** IO3c's commit on `3aa37c00`.
**Goal:** retire the open-time warning "the LARD reaction binding is not
implemented (deferred L3)" — the last unstarted quality step.

## 1. Scoping facts (measured 2026-08-31, this session)

1. **MSX species do not ride LARD segments today.** `rowLayout()`
   (`LagrangianSolver.hpp:150`) is pollutants + age + temp. The status
   doc's "L3 is a binding, not a new module" understates by one layer:
   the *store* must first carry the rows.
2. **The reference bar is lower than it looks: LEGACY does not transport
   MSX species either** (`ReactionData.hpp:136` — "react per element but
   are NOT yet transported between elements under LEGACY (R4b)"). Only
   ARD transports them (on its mesh). LARD with segment-resident species
   MATCHES ARD's ambition, not merely legacy's.
3. **The store is row-generic**: `SegmentStore::resize(n_links, ns)`,
   slab layout, `decay_species(row, f)`; H7a made the layout explicit
   (`SpeciesRowLayout`), H7b proved a row can be added on top of a
   trusted layout without disturbing the others — L3a repeats that move.
4. **Species mass enters under LARD via initial state only**
   (`rx.init_global` / `init_elem_*`): [TRANSPORT_BOUNDARIES]/[SOURCES]
   are ARD-engine content (warned inert under LARD — keep that warning),
   and MSX names take no [INFLOWS]. Kinetics coupling to pollutants
   (which DO take inflows and transport) is the live pathway — FORMULA
   and RATE species reading `pollut` context.
5. The shared integrator is `ReactionIntegrator::step(rx, tank, dt,
   block, hydvar, ws, pollut)` — the legacy binding shows the
   per-element gather/scatter (`reactElements`,
   `ReactionLegacyBinding.cpp`); the ARD binding shows cells + stores
   and full hydraulic-variable population (R6).

## 2. The work — one round, two movements

**L3a — species rows in the store.** `rowLayout()` appends `n_msx` rows
(`msx_first = ns; ns += rx.n_species()`) when a reactions component is
configured and compiled. Seed segments and node stores from the initial
conditions at `init()` (mirroring `msx_node_conc`/`msx_link_conc`
initialisation in `ensureMsxState`). The existing stages carry the rows
with NO new physics: AGE touches only `age_row`, DECAY touches only
pollutant rows, MIX/DRAIN/RELEASE are row-generic. RWPT: give species
rows the pollutant dispersion treatment (the H7b temperature precedent —
say which coefficient family and why). `publish()` scatters the rows to
`rx.msx_link_conc` / `rx.msx_node_conc` (volume-weighted like
pollutants), so the API and .out surfaces read identically to LEGACY.

**L3b — the react stage.** After DECAY, per SEGMENT: gather the species
rows into the stack block, `ReactionIntegrator::step` with the segment's
pollutant rows as `pollut` context (tank=false), scatter back; per NODE
STORE: the same with tank=true and the node's HRT. Then delete the L3
warning at `SWMMEngine.cpp:347` — a warning for a thing that now runs is
worse than none.

## 3. Protocol

1. **Gates must FAIL at base:**
   - `LardMsxSpeciesReactOnSegments`: stagnant/level-pool deck, RATE
     species `dX/dt = -k·X`, initial X = C0 — under LARD at base, X
     stays C0 (or is absent); patched, X tracks `C0·exp(-k·t)` within
     the integrator's tolerance. Quote the base value.
   - `LardFormulaSpeciesTracksItsPollutant`: FORMULA Y = 2·TSS on a
     flowing deck — base: Y dead; patched: Y = 2·TSS along the chain.
   - The open-time warning is GONE on a reacting LARD deck (and the
     bypass warnings that should remain — treatment — still fire).
2. **Bit-identity where nothing changed**: a LARD deck with NO reactions
   component must be BYTE-IDENTICAL to base (`.out` + `.rpt`) — the
   rowLayout change must be provably inert when n_msx = 0 (H7a's own
   standard). Corpus 21/21 (heat_lard carries no reactions — confirm).
3. ctest ×3 vs 184 + the new gates. The LEGACY react-in-place suites
   (test_reaction_legacy_binding) must be untouched.
4. **Cross-engine leg**: the same reacting deck under EULERIAN_ARD —
   L3's segment answer should agree with ARD's cell answer to within
   discretisation (report the number; exactness is NOT expected).
5. **Falsifiers:** (i) skip the scatter (integrate into a scratch copy)
   → both gates read initial values; (ii) gather pollutant rows into the
   species block (index off by np) → FORMULA gate reads garbage; (iii)
   tank=true for segments → HRT-dependent expressions misbehave (pick a
   deck where it shows, or record why unobservable); (iv) n_msx = 0
   layout regression → the bit-identity leg catches any drift.

## 4. Owed to the record

The `[TREATMENT]`-under-LARD warning stays (P2.3). The ARD BC/source
inertness warning stays. Wire nothing else.

---

# IMPLEMENTATION + CHECK RECORD — 2026-08-31, same session

**Verdict: IMPLEMENTED and COMMITTED as engine `ec22580a`** (on `183f59f3`,
6 files). Evidence: `tests/output/l3_lard_reactions/`.

One round, not two: §1's scoping facts held exactly (the store was
row-generic; L3a was mechanical thanks to H7a/H7b). Both gates failed at
base with the species arrays EMPTY. Protocol results: wiring 7/7; 30/30
affected; **184/184 ×3 (+1 on final gate text)**; **corpus 21/21**
(heat_lard = the n_msx==0 bit-inertness witness). FORMULA within 2%;
**cross-engine ratio 0.9751** (LARD 85.4627 vs ARD 87.6447 at C5).

§3.5 falsifiers, with one check-round correction: the off-by-np gather
read ratio **1.1461 — inside the drafted 0.35 band and inside 0.15 by
0.004**; the shipped band is **0.10, calibrated by the falsifier**
(lesson 210). Seeding-drop bites by crash (unreachable in real code —
init always seeds). §3.5.iii (tank=true for segments) recorded
UNOBSERVABLE on these decks: both fixtures define PIPES and TANKS scopes
identically, and no gate reads node-species state — a future deck with
divergent scopes would observe it.

Owed onward: treatment under LARD (P2.3) keeps its warning; per-species
LARD dispersion coefficients ride the pollutant treatment (H7b's
temperature precedent, noted in the RWPT comment).
