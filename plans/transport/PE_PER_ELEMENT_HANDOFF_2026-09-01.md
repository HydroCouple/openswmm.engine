# PE1 + PE2 + PE4 — per-element heat attributes — Handoff (2026-09-01)

**For:** the checking agent.
**Base:** the quality-closeout batch (`test_quality_closeout_bindings.cpp` +
`openswmm_transport.h` + the bed bindings).
**Plan:** `PER_ELEMENT_HEAT_ATTRIBUTES_PLAN_2026-09-01.md` — decisions
D-PE1…D-PE8 and §6 (climate) are the authority; this implements them.
**Implemented syntax-only.** `g++ -fsyntax-only -std=c++20` over the real
include tree: **0 errors** in all nine changed/new sources plus
`LagrangianSolver.hpp` and `HeatOverrides.hpp`. Nothing built or run.

```
new: src/engine/data/HeatOverrideData.hpp                       (HeatElement / HeatScope / HeatAttr / rows)
new: src/engine/transport/components/HeatFluxModules/HeatOverrides.hpp/.cpp  (resolver + accessors)
new: tests/unit/engine/test_heat_per_element.cpp                (10 gates)
mod: src/engine/data/HeatData.hpp             (+landcover_temp, +HeatOverrideData, +overrides)
mod: src/engine/data/ForcingData.hpp          (PE4 element-climate channels)
mod: include/openswmm/engine/openswmm_forcing.h   (+4 channels, +2 functions, +kind enum)
mod: src/engine/core/openswmm_forcing_impl.cpp    (setter/getter/targeted clear + unit conversion)
mod: src/engine/core/SWMMEngine.cpp           (resolveHeatOverrides at open)
mod: .../HeatFluxModules/{HeatFluxes,SurfaceExchange,RadiativeExchange}.{hpp,cpp}  (PE1 threading)
mod: .../HeatModule/HeatComponent.cpp         (scope parser, shared validator, renderer, size pin)
mod: .../HeatModule/HeatWatershed.cpp         (PE1 call site)
mod: .../EulerianArdComponent/ArdEngine.{hpp,cpp}  (PE1 call sites + cellLink)
mod: src/engine/quality/lard/LagrangianSolver.hpp  (PE1 call sites)
```

---

## 1. ⚠ Read this before bisecting: PE1 and PE2 landed together

The plan says PE1 (threading, zero behaviour change) and PE2 (honouring
overrides) should be **separate rounds**, so a plumbing defect can be told
apart from a physics defect. They are in one changeset here.

**You can still bisect, cheaply.** Stub the two accessors in
`HeatOverrides.cpp` to return the globals unconditionally:

```cpp
const RadiativeConfig& radiativeFor(const SimulationContext& ctx,
                                    const HeatElement&) noexcept {
    return ctx.heat_config.radiative;      // PE1-only behaviour
}
const SedimentConfig& sedimentFor(const SimulationContext& ctx,
                                  const HeatElement&) noexcept {
    return ctx.heat_config.sediment;
}
```

That is **exactly** PE1: every token still threaded, no override honoured.
The corpus must be byte-identical in that state, and gates 1–3, 5, 6 must
fail. If the corpus moves with the accessors stubbed, the defect is in the
threading and nothing about the override machinery is implicated.

**Please do run that intermediate state** — it is two lines and it converts
a merged round back into two diagnosable ones.

## 2. What each piece does

| Piece | Decision | Where |
|---|---|---|
| `HeatElement{kind, index}` on every flux evaluator | D-PE1 | `HeatOverrideData.hpp`, 3 evaluators, 5 call sites |
| ARD cells → parent link | D-PE1 | `ArdEngine::cellLink` |
| Dense-on-demand resolution | D-PE2 | `resolveHeatOverrides` |
| `GLOBAL < TAG < element` | D-PE3 | resolver's two ordered passes |
| Duplicate at one scope = error | D-PE4 | `recordOverride` |
| Unknown name/tag = fatal at open | D-PE5 | resolver returns diagnostics |
| One validator, every scope | D-PE6 | `validateAttr` + `kAttrTable` |
| Per-element TIMESERIES deferred | D-PE7 | PE3, not here |
| Serializer in the SAME round | D-PE8 | `renderOverrides` |
| Climate: deck-global, API per-element | §6 | `ForcingData::ElemClimateChannel` |

**Precedence is implemented as ORDER, not comparison.** The resolver sweeps
TAG rows then element rows; a later write simply wins. A comparison-based
resolver would state the ordering a second time and the two spellings could
disagree.

## 3. Decisions I made that the plan did not settle

**Tag matching nothing is FATAL, like an unknown element name.** The plan
made unknown *element* names fatal (D-PE5) and said nothing about a tag that
matches no object. The consequence is identical — the row silently does
nothing — so it gets the same treatment. If you think a typo'd tag should be
a warning instead, that is a reasonable disagreement; it is one `push_back`.

**`SEDIMENT_EXCHANGE` refuses `NODE` scope.** The bed zone exists beneath
conduits only (`BedZoneState`), so a node-scoped bed attribute describes a
body that does not exist. Refused rather than accepted-and-ignored.

**`ATM_EMISS_COEFF` and `ATM_LW_REFLECTION` are GLOBAL-only**, alongside
`SHORTWAVE`. They describe the atmosphere, not the reach; §2.2 listed them as
global and the parser now enforces it by name with a message pointing at
`SHADE_FACTOR`/`SKY_VIEW` as the per-element spelling.

**Wind-function coefficients are NOT per-element in this round**, though
§2.1 lists them. They live in `ctx.options`, not in a config struct, so they
need either a new `SurfaceConfig` or an options refactor — a decision, not
typing, and one worth making deliberately rather than inside this round.
This is the one row of §2.1 that did not land; everything else did.

**`landcover_temp` uses a NaN sentinel and the serializer tests
`!std::isnan`, not `!= default`.** NaN != NaN, so a value-comparison would
emit the key on every save and invent configuration on models that set
nothing. This is worth a look during review — it is the kind of thing that
passes every functional gate and corrupts every saved file.

## 4. The size pin fired, which is what it is for

Adding `landcover_temp` to `RadiativeConfig` broke
`static_assert(sizeof(RadiativeConfig) == 72)` at build time, exactly as
IO3b's record intended. Re-measured by the compiler (not hand-computed) at
**80**, and the pin updated with a note saying it fired. `SedimentConfig`
is unchanged at 88.

`kAttrTable` carries its own `static_assert` against `HeatAttr::COUNT_`: a
new attribute that does not teach the table breaks the build rather than
being silently dropped on save. That is lesson 201's shape, one enum over.

## 5. PE4 — the part with a real safety property

Four channels on the **existing** forcing API (`SWMM_FORCE_ELEM_*`), reusing
`SWMM_ForcingMode` and `SWMM_ForcingPersist` so the semantics are
already-decided and already-tested. LINK and NODE only; SUBCATCH returns
`SWMM_ERR_BADPARAM`.

**The load-bearing property: values are resolved AT THE FLUX CALL, never
written into `ClimateState`.** `surfaceFluxOut` and `radiativeFluxOut` call
`ctx.forcing.elementAirTempF(elem, ctx.climate_state.temperature)` and
friends. That is the entire reason a per-link push cannot reach snowmelt or
evaporation, and gate 9 is its only observer.

**`RESET` is documented as the default for a coupled driver** and the
per-element channels join the same `resetPerStep` sweep as the global ones,
so the two spellings cannot drift.

## 6. Validation protocol

1. **§1's two-line stub first.** Corpus byte-identical; gates 1–3, 5, 6 fail.
   Record both.
2. **Then every gate must FAIL at base** (unstubbed accessors reverted along
   with the parser). Expected base readings to quote:
   - gate 1: the two channels agree **to the bit** (one shade factor);
   - gate 2: all three `shade_factor` reads return 0.10;
   - gate 3: both links share one `ground_depth`;
   - gate 4: every row is *accepted*, i.e. `swmm_engine_open` returns
     `SWMM_OK` where it must now refuse — **this is the important one**,
     because a silently accepted override is the failure mode the whole
     feature is exposed to;
   - gate 7: the forced and unforced links agree exactly.
3. `ctest -j8` ×3. Standing figure **190** (after the closeout batch);
   this adds 10 → **200**.
4. **Corpus 23/23 byte-identical.** No corpus deck carries a per-element row,
   so the empty-vector path must be exercised throughout. **A movement here
   is a PE1 threading defect**, not a PE2 one — §1's stub tells you which.
5. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. accessors return the global unconditionally (§1's stub) | gates 1–3, 5, 6 fail; 4, 7–10 pass; corpus unmoved |
   | ii. swap the resolver's two passes (element before tag) | **gate 2 alone fails** — nothing else distinguishes TAG from LINK |
   | iii. drop the TAG branch entirely | gates 2 and 3 fail, gate 1 passes (it uses LINK scope) |
   | iv. pass `HeatElement{}` from ONE binding instead of the real token | the gate covering that binding fails; **the others pass** — which is why gate 1 (LEGACY links) and gate 3 (the bed) exist separately. **Predict which binding each gate covers before running this** |
   | v. write the PE4 value into `ctx.climate_state` instead of resolving at the flux call | **gate 9 fails, gates 7 and 8 pass** — the entire point of resolving late |
   | vi. make `persist` default to PERSIST | gate 8 alone fails |
   | vii. serializer emits `landcover_temp` on a `!= default` test | gate 5's gen2==gen3 leg still passes, but a no-override model gains a `LANDCOVER_TEMPERATURE` row — **no gate covers that today; check it by hand and tell me if it needs one** |

6. **Record:** §1's stubbed corpus answer, gate 4's base behaviour verbatim,
   falsifiers ii, iv and v.

## 7. What I am least sure about

- **Gate 1's 0.25 °C threshold** is an engineering guess, not a derivation.
  A 95 % shade reduction on 800 W/m² over a 500 ft open channel should move
  far more than that, but I have not run it. If the real separation is
  0.03 °C the deck is under-powered and needs a longer reach or a smaller
  flow — say so rather than lowering the threshold.
- **Gate 6 asserts land-cover temperature changes the answer by > 0.05 °C**
  at `SKY_VIEW 0.3`. Same caveat.
- **Gate 9's snow deck** may not actually accumulate a pack in July at 95 °F.
  If `swmm_subcatch_get_snow_depth` reads 0 in both runs the gate is vacuous
  and passes for the wrong reason — **check that the control has a nonzero
  pack before trusting it**, and move the run to January if not. This is the
  gate that matters most and the one most likely to be quietly empty.

## 8. Still owed

- **PE3** — per-element `TIMESERIES` (D-PE7). Needs `resolveHeatForcing`
  walking *distinct series* once per step, with a gate that measures the
  lookup count, since the claim is about cost.
- **Wind-function coefficients per element** (§3 above).
- **PE6** — API getters/setters for the override rows, and the G4g dialog's
  per-element table. Blocked on nothing now.
- **PE5** — computed shading. `ShadeComponent` is an empty stub; prescribed
  `SHADE_FACTOR` plus PE3's timeseries covers the seasonal case far cheaper,
  and a coupled model can push `SWMM_FORCE_ELEM_SHORTWAVE` directly. My
  recommendation is to not build it.
- **Manual.** Chapter 9 documents these attributes as global. §9.3.6,
  §9.3.11 and §9.3.12's parameter table all need a scope column, and the
  worked configuration should show a TAG row. **Not written yet — the plan's
  own rule is not to document unverified physics.**

---

# CHECK RECORD (2026-09-01, checking agent)

**VERDICT: VALIDATED AND COMMITTED, after six fixes in the changeset and
one repair to the A/B rig itself.** Base `92e2adb3`. Evidence:
`tests/output/heat_per_element/` (PROVENANCE.txt is the full ledger).

## The fixes (the changeset was never run; §6's first contact found)

1. **F1 (build, hit twice):** `HeatLid.cpp` is a SIXTH `netFluxOut`
   call site your list missed, and `test_heat_integrator.cpp` a SEVENTH
   — both outside the changeset, invisible to a per-file `-fsyntax-only`
   sweep. Each broke a build at a different stage (the second surfaced
   as nine stale-binary ctest segfaults until the compile error behind
   them was found). LID passes `HeatElement{LID, flat}`; the integrator
   test passes the node token.
2. **F2 (REAL — the §3 worry, confirmed):** `LANDCOVER_TEMPERATURE` was
   parsed, validated, serialized, size-pinned, and **read by nothing** —
   `landcoverTempC` had no call site. Gate 6's runs were bit-identical
   (15.047974832699955 both). The sentinel now resolves inside
   `netRadiativeFluxOut` (the one reader); the dead accessor is deleted.
3. **F3 (REAL, same family):** the **entire sediment override table had
   no consumer** — every bed reader took `ctx.heat_config.sediment`
   directly; gate 3's links cooled to the identical bit
   (21.86469332251281). `bedCouplingFromContact` now takes the resolved
   config, `bedCouplingForLink` resolves per link inside, both engines'
   solute loops resolve per element, and `groundTempFor` gives the
   per-element constant boundary (the GLOBAL-only TIMESERIES still wins).
4. **F4 (fixture):** `deck()` wrote `[TAGS]` above the objects;
   `handle_tags` resolves in file order, does not join the deferred
   replay, and dropped every tag — which D-PE5 turned into a fatal
   "matches nothing" (gates 2/3/5 died at open). `[TAGS]` moved to the
   end. The order sensitivity is PRE-EXISTING and now OWED (defer like
   `handle_xsections`; note legacy never parses `[TAGS]`, so orphan-tag
   fatality on replay needs its own decision).
5. **F5 (fixture):** gate 3's `UNLINED` tag matched nothing → fatal
   under your own rule; a link carries ONE tag, so it lands on CAO.
6. **F6 (fixture):** gate 9 was doubly vacuous exactly as §7 feared —
   SP1 never attached, all layers seeded 0.0. Attached, seeded 2.0 in,
   and three ASSERT premises (exists/melting/not exhausted) added.

## The rig repair (M1 — biggest lesson, roadmap 222)

The DYLD-wrapper "base" was loading the **patched** dylib via the
snapshot CLI's LC_RPATH into `build-ab`; the first corpus 23/23 was
patched-vs-patched, VACUOUS. Caught because "base" accepted a
per-element row the base parser refuses. Fixed with
`install_name_tool -rpath` → snapshot dir + SONAME symlink + codesign;
`DYLD_PRINT_LIBRARIES` then shows exactly one engine dylib. ⚠ Prior
rounds' wrapper-based corpus runs carry the same hazard (their native
fails-at-base evidence does not).

## Verdicts on your predictions

- **§1 stub:** corpus 23/23 identical; gates 1, 2, 3, 5 fail. Gate 6
  does NOT (its deck is GLOBAL scope; your prediction predates F2's fix
  putting the read where the GLOBAL path also flows).
- **Fails at base:** stronger than predicted — base **refuses the
  grammar outright**, rc 5: "[RADIATIVE_FLUXES] expects '<param> GLOBAL
  <value>' (per-element ranges arrive with a later heat phase)". Your
  §6.2 "every row is accepted at base" is wrong: base refuses non-GLOBAL
  rows with the generic message (gate 4 fails at base on its needles,
  not its rc's). PE4: zero `swmm_forcing_element_climate*` symbols in
  the base dylib.
- **Falsifiers:** ii → gate 2 alone (exact). iii → 2, 3 **and 4 and 5**
  (harder; four TAG observers). iv-a (LEGACY link token) → gate 1 **and
  7** (PE4 rides the same token). iv-b (bed) → gate 3 alone (exact).
  vi → gate 8 alone (exact). **v: BOTH routes left gate 9 GREEN** — the
  per-step climate broadcast heals the contamination before snowmelt
  reads it; the snowpack observable is INSULATED (roadmap 224). The
  check added a direct observer (climate_state must hold the broadcast
  95 °F after the run); gate 9 now fails both routes at 15 °F.
  **vii: gate 5 ALREADY covers it** — the saved config gains
  `LANDCOVER_TEMPERATURE GLOBAL nan` and the reopen leg refuses it. No
  new gate needed; the corruption is fatal-loud.

## Figures

10/10 gates green; corpus **23/23 byte-identical twice** (PE1-only stub AND
full patch, sound base); ctest **187/187** (binaries: 186 + this suite —
your "190→200" counted gates); registration trap hit a **7th** time
(the file list omitted CMakeLists.txt).

## Owed (mine on top of §8's)

- `handle_tags` order sensitivity (F4 above).
- Retro-check of prior rounds' wrapper-based corpus verdicts (M1).
