# Kdecay units — TRIAGE RECORD + fix handoff (KD1) — 2026-08-31

**Status: TRIAGED and CONFIRMED.** The debt H7b flagged and five handoffs
carried is real, it is the full 86 400×, and it is worse than flagged — the
common case does not merely decay too fast, it **destroys the pollutant mass
without booking it anywhere**. Evidence: `tests/output/kdecay_triage/`
(decks + both engines' `.rpt` + PROVENANCE). Triage base: `23c1ddfb`.

---

## 1. The finding, statically

The `[POLLUTANTS]` Kdecay column is **1/day** in the file format. Legacy
converts at parse — `landuse.c:173`:

```c
Pollut[pollutIndex].kDecay = x[3]/SECperDAY;   // stored 1/sec
```

— and every legacy application site multiplies the stored 1/sec value by a
tStep in seconds (`qualrout.c:438` exponential on links, `:613` linearized on
nodes). The legacy API exposes the value back in 1/day (`swmm5.c:1567-1618`,
`* SECperDAY` on get, `/ SECperDAY` on set).

The new engine parses the column **raw** (`QualityHandler.cpp:132`,
`k_decay[idx] = to_double(tok[5])` — and `PollutantData.hpp` documents the
field as 1/day) and then multiplies that raw value by **dt in seconds** at
every application site, with **no /86400 anywhere** between parse and use:

| site | form |
|---|---|
| `QualityRouting.cpp:775` (`applyDecay`, nodes) | `1 − k·dt` linearized |
| `QualityRouting.cpp:850` (link mixing / steady) | `1 − k·dt` and `exp(−k·dt)` |
| `ReactionLegacyBinding.cpp:113` (`decayPollutantsExact`) | `exp(−k·dt)` |
| `ReactionArdBinding.cpp:104` (ARD cells + stores) | `exp(−k·dt)` |
| `LagrangianSolver.hpp:445` (LARD DECAY stage) | `exp(−k·dt)` |

The C API (`swmm_pollutant_set/get_kdecay`) stores and returns raw, so GUI
and Python edits ride the same wrong unit. GeoPackage round-trips raw
(consistent with itself, wrong with everything else).

**Every quality engine is self-consistent and all of them are 86 400× too
fast against the file format and the reference.**

## 2. The finding, empirically (differential, legacy CLI vs new CLI)

One junction + one conduit + outfall, 1 cfs constant inflow at 100 mg/L TSS,
6 h run, 30 s routing step. Quality Routing Continuity (lbs):

| run | ext outflow | mass reacted | continuity error |
|---|---|---|---|
| legacy, Kdecay 1.0 | 133.988 | 0.159 | −0.097 % |
| **new, Kdecay 1.0** | **0.000** | **0.000** | **100.000 %** |
| legacy, Kdecay 0 (control) | 134.146 | 0.000 | −0.097 % |
| new, Kdecay 0 (control) | 133.917 | 0.000 | 0.216 % |
| new, Kdecay 1/86400 (ratio quote) | 133.667 | 0.000 | 0.402 % |

- Controls agree → the entire split traces to the Kdecay column.
- Dividing the deck value by 86 400 restores legacy-magnitude decay → the
  ratio is exactly the units factor, nothing else.
- Legacy at k = 1/day is a **0.1 % effect**; the new engine **zeroes the
  entire run's outflow**. Any user deck with a plausible Kdecay (0.05–1/day
  BOD/coliform values) has been silently reporting ~zero for that pollutant.

## 3. Defect (b), found by the same probe: the legacy-path solver never books decayed mass

`QualitySolver::applyDecay` and the in-mix link decay **do not book into
`qual_routing_reacted`** — legacy books it (Mass Reacted 0.159); the new
run's decayed mass surfaces as *continuity error* (0.402 % vs control's
0.216 % in the scaled run; 100 % in the raw run). Two aggravations:

1. `1 − k·dt` with k·dt = 30 gives **−29 → clamped to 0**: total
   annihilation with no ledger row and no warning.
2. ARD (`ReactionArdBinding`) and LARD (`LagrangianSolver` DECAY) **do**
   book their `removed` mass — so the booking hole is specific to the
   legacy-path `QualitySolver` (both its node pass and its link-mixing
   decay), read statically; the empirical runs above used KINWAVE + the
   legacy-path solver.

## 4. Fix round (KD1) — scope

1. **Convert at parse, mirror legacy exactly**: store 1/sec
   (`k_decay[idx] = x/86400`), update the `PollutantData.hpp` doc comment.
2. **Every seam that speaks "file units" converts back**: `InpWriter`
   (×86400 on the Kdecay column), GeoPackage writer (decide: store file
   units in the GPKG — pick whichever the GPKG schema doc claims, and gate
   the round trip), GeoPackage reader to match.
3. **API unit contract — a decision, record it**: legacy's API exposes
   1/day. `swmm_pollutant_get/set_kdecay` today exposes "whatever is
   stored". Recommend **1/day at the API** (matches legacy
   `swmm_POLLUT_KDECAY`, matches what a GUI shows beside the deck), with
   the conversion inside the impl. Audit GUI editors + MCP + gymnasium for
   places that display or set it.
4. **Book legacy-path decayed mass** into `qual_routing_reacted` (nodes and
   link mixing), and clamp `1 − k·dt` at 0 **while still booking the true
   removed mass** — after the units fix, k·dt is ~3×10⁻⁴ for k = 1/day at
   30 s, so the clamp becomes unreachable in sane decks, but it must not
   silently annihilate when reached.
5. **Gates**: (i) a differential-style gate — deck k = 1/day over a known
   residence time loses the analytically expected ~0.1 %, not everything
   (fails spectacularly at base); (ii) write→reopen round-trip: Kdecay
   column value survives (would fail if only parse converts); (iii) API
   get returns the deck value (pins the contract of §4.3); (iv) ledger:
   Mass Reacted > 0 and continuity error small on the legacy path.
6. **Corpus**: any corpus deck with nonzero Kdecay **will move — correctly**.
   Check which ones before the A/B and record expected movement (this is
   the rare round where "moved" is the passing result; quote the ledger
   improvement per moved deck).
7. **Falsifiers**: drop the parse conversion → gate (i) fails; drop the
   writer conversion → gate (ii) fails 86 400× small; drop the API
   conversion → gate (iii); book nothing → gate (iv).

**Size: 1 round.** The only genuinely open call is §4.3's API contract.

## 5. Why five handoffs of "corpus 21/21" never caught it

The corpus A/B compares the same engine against itself — a units error
present on both sides is invisible. No parity gate runs a decaying
pollutant against the legacy CLI. Gate (i) of KD1 closes that class.

---

# IMPLEMENTATION + CHECK RECORD (KD1) — 2026-08-31, same day

**Verdict: IMPLEMENTED and COMMITTED as engine `3aa37c00`** (on `5015810d`;
16 files; tree 1941). Evidence: `tests/output/kd1_kdecay_units/`.

## §4 executed — with the API decision and TWO more defects found en route

§4.1–4.3 as written: parse stores 1/sec (legacy's seam), INP/GPKG/API
convert at every file-unit boundary. **§4.3 decided: the API speaks 1/day**
— which the header, the MCP docstring, and legacy's `swmm_POLLUT_KDECAY`
all already promised; only the impl dissented. GUI/MCP/gymnasium are
symmetric API pass-throughs and needed ZERO changes. The existing unit
gates and the CSTR benchmark set `k_decay` directly in 1/s — under
store-1/sec they pass UNTOUCHED, which is what settled §4.1's design over
the store-1/day alternative. Test deck writers that wrote spec-1/s values
now write `value*86400` (they pinned the broken parse, not the physics).

**Defect found by the booking gate (beyond §4.4's list): every node
decayed.** Legacy decays only STORAGE nodes or nodes actually holding
volume (`routeQuality` dispatch; `findNodeQual` decays NOTHING) — the new
engine decayed pass-through junction flux, unbookable from any volume
basis and a real divergence (outflow 30.9 vs legacy 35.9 at k=200/day).
The node loops now carry legacy's gate; booking uses the CURRENT volume
(the post-mix conc's pairing — an old-volume basis overbooked a draining
storage by 21.7%, measured).

**Defect found by the storage gate: `mixAtNodes`' evaporation factor
created mass.** It inferred evaporation from `v_new < v_old + v_in` —
true for EVERY draining node — inflating the store to retain mass the
downstream link had already pulled; the `min(c_new, c_max)` cap masked
the creation whenever concentrations were uniform, i.e. on every k=0
deck ever run, including the whole corpus. A draining storage under
KDECAY created ~`c·v_out` per step (**−47% continuity, measured**). Now:
`fEvap = 1 + vEvap/v1` from the storage unit's ACTUAL `evap_loss`
volume, per legacy `findStorageQual`; cap removed (legacy has none; the
volume-balance mix cannot exceed `max(c_old, c_in)` on its own).

## Protocol results

| step | result |
|---|---|
| gates at base | fail quoting the loss: outfall 0 of 100 mg/L ("applied as 1/sec"), continuity error **100%**, Mass Reacted 0.000 |
| patched | 12/12 suite; 30/30 affected suites; **ctest 184/184 ×3** (twice — before and after the evap repair); **corpus 21/21** both artifact kinds, re-earned after the mixAtNodes change (bit-inert: the cap-masked regime covered all 21) |
| cross-engine | k=1/day: Mass Reacted **0.158 vs legacy 0.159**; k=200/day: **8.455 vs 8.523**, error 0.575% vs −0.348%; sealed tank books exactly (−0.000%) |
| falsifiers | all six bite (`falsifiers.log`): parse → annihilation; writer → column **0.0000** (`%10.4f` erases 1/sec); API get → contract gate; link booking → 19.4%; node booking → 43.5%; evap revert → **−47.4%** |

## Residual, recorded (P2.4's class, BOTH engines)

Per-step transit mass takes the decay factor outside any volume-basis
booking. On the storage gate deck: new +11.9% vs LEGACY's +6.5% (at k=0:
+3.9% vs legacy +7.0% — the reference leaks MORE than we do there). The
storage gate's threshold (15%) quotes both baselines in place.

## Check-round note on the falsifier sweep itself

The first sweep silenced build output and did not verify restoration — a
failed restore left F-i applied under F-ii…F-v, contaminating their
bites (observed: the "restored" tree still annihilated). Redone hardened:
cp-backup per falsifier, build rc checked, tree `cmp`-verified against
reference snapshots after every restore, final full-suite green on the
verified-clean tree. **A falsifier sweep needs its own falsifier: the
restore step.** (Roadmap lesson 207.)
