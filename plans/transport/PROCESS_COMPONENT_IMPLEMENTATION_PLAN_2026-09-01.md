# Process Component (HydroCouple) Implementation Plan — 2026-09-01

**Phase 2 of the unified transport program, fleshed out.** Supersedes the
four-line HC1–HC4 rows in `IMPLEMENTATION_ROADMAP.md` §Phase-2 and the E7 row;
those rows now point here. `PROGRAM_REVIEW_2026-08-25.md` §3 is the honest
starting statement and nothing in it has changed since:

> The entire `[PROCESS_COMPONENTS]` machinery is an **in-process string-id
> registry** with HydroCouple-*flavoured* naming. No ABI, no dynamic loading,
> no HydroCouple types.

**The refusal to retire** is `ProcessComponentRegistry.cpp:217-224`: a
`[PROCESS_COMPONENTS]` row whose id looks like a library path hard-errors with
*"library-loaded HydroCouple components are not available yet (arrives with
plan phase HC2)"*. That error message is this plan's contract: every phase
below narrows what it refuses, and HC2 is where it starts admitting.

---

## 0. What exists, measured (2026-09-01)

| Asset | State |
|---|---|
| `[PROCESS_COMPONENTS]` parse + registry + apply/save hooks | ✅ live — five in-process components (`reactions`, `transport.ard`, `heat`, `waterage` + refusing stubs) |
| `ComponentConfigApply` / `ComponentConfigSave` seam (IO3a–c) | ✅ live — components read AND write their own config files |
| Plugin loader (`PluginFactory`) | ✅ live for `openswmm_plugin_info` plugins (INPUT/OUTPUT/REPORT/STATE_IO); `dlsym`, sandboxed warnings, RC.2 leak fix |
| HydroCouple headers in the engine build | ⛔ zero references — the `HydroCouple` checkout is not in any build file |
| `PluginType::PROCESS_COMPONENT` | ⛔ absent (four enumerators only) |
| `IModelComponent` in engine source | ⛔ zero occurrences outside `paper/paper_v2.md` |
| Reference implementations | ✅ `HydroCouple/SWMMComponent` (legacy SWMM as an IModelComponent), `CSHComponent`, `HTSComponent`, `GWComponent`, Composer, `HydroCoupleSDK` |

**Read before implementing HC3:** `HydroCouple/SWMMComponent` wraps *legacy*
SWMM as an `IModelComponent` — it is the closest thing to a worked answer for
every wrapper question below (status events, exchange-item construction,
update loop), written by the same author against the same engine family.

## 1. The two directions, named explicitly

"HydroCouple integration" is TWO capabilities that share headers and nothing
else. Conflating them is how the original four-line phase became uncostable.

- **Direction A — openswmm AS a component (E7/HC3).** Wrap the engine in
  `IModelComponent` so the Composer can couple it to CSHComponent, GWComponent,
  CE-QUAL-W2, etc. The engine is a *server* of exchange items. This direction
  needs **no dynamic loading in the engine at all** — the component library
  links against the engine's C API and HydroCouple headers, and it is the
  *Composer* that loads it.
- **Direction B — components INSIDE openswmm (HC2).** A `[PROCESS_COMPONENTS]`
  row names a shared library; the engine loads it and drives it per routing
  step, so an HTS bed zone or a CSH stream-temperature model can replace an
  internal module deck-by-deck. The engine is the *host*. This is what the
  refusal currently refuses.

**Decision D-PC1: Direction A ships first.** Three reasons. (1) It produces
coupled *results* (heat/GW compositions, HC4) without designing a hosting ABI.
(2) Its artifact — an `IModelComponent` over the C API — is exactly the shim
Direction B's loaded components must be driven through, so A's wrapper is B's
test double. (3) The riskiest unknowns (time negotiation, exchange-item
geometry) get exercised where the Composer already handles orchestration,
instead of inside our routing loop. Direction B retires the refusal only at
HC5, and the refusal's message changes at each phase so it never lies.

## 2. Phases

### HC1 — headers + build plumbing (small, land any time)

- `find_package(HydroCouple)` / FetchContent fallback; the four interface
  headers (`hydrocouple.h`, `temporal`, `spatial`, `spatiotemporal`) on an
  INTERFACE target `openswmm::hydrocouple_iface`. Header-only: no linkage.
- A `OPENSWMM_WITH_HYDROCOUPLE` option, OFF by default, so nothing here can
  move the corpus. Everything below sits behind it.
- **Verify:** configure + compile a TU that instantiates nothing but includes
  everything, both option states, all three platforms CI covers.

### HC2a — the hosting seam, WITHOUT dynamic loading

Define `IProcessComponentHost` in `src/engine/plugins/`: the bridge between
the routing loop and *any* externally-implemented component. One instance per
`[PROCESS_COMPONENTS]` row.

```
struct ProcessComponentBinding {
    // Called at open, after the full .inp parse (the D-RQ1 timing).
    virtual std::vector<std::string> bind(SimulationContext&) = 0;
    // Called once per routing step, AFTER hydraulics, BEFORE quality
    // publication. `dt` is the routing step; the component subcycles
    // internally (HTS/CSH both do).
    virtual void step(SimulationContext&, double dt) = 0;
    virtual void finish(SimulationContext&) = 0;
};
```

The four in-process components do NOT migrate onto this seam — they are
config appliers, not steppers, and forcing them through it would be churn
with no observer. The seam exists for what comes from outside.

- **Verify:** a test-only `ProcessComponentBinding` that doubles a pollutant
  concentration per step, registered by id, observable through the C API.

### HC2b — exchange mapping tables (the real design work)

What state crosses, in which units, on which geometry. One table per
direction, versioned in this plan, reviewed BEFORE code. Initial surface —
deliberately the smallest set that lets HC4's two compositions run:

| openswmm state | direction | HydroCouple item | geometry |
|---|---|---|---|
| `links.flow`, `links.depth`, `xsect area` | out | `IOutput` time-geometry series | link polylines |
| `heat_state.link_temp` | out/in | temperature series | link polylines |
| `nodes.depth`, `nodes.volume` | out | series | node points |
| `ext_inflows` (lateral flow + conc) | in | `IInput` | node points |
| `bed_state.link_temp` (H6b) | out/in | series | link polylines |
| climate (`temperature`, `wind`, `rh`, `Jin`) | in | global series | scalar |

Unit rule: the boundary speaks SI (HydroCouple convention); the engine's
ft-s internals convert at the wrapper, once, with the `kSqFtToSqM` naming
convention. **No exchange item may bypass the C API** — if the wrapper needs
state the API does not expose, the API grows a getter first (the E6/step-3
precedent), because that keeps the coupled surface identical to the scripted
surface.

### HC3 / E7 — openswmm as `IModelComponent` (Direction A lands)

New top-level target `src/couplers/hydrocouple/` (NOT under `src/engine/` —
it links the engine, includes Qt-free HydroCouple headers, and must be
excludable in one line):

- `OpenSWMMModelComponent : IModelComponent` — lifecycle mapping:
  `initialize()` → `swmm_engine_open+initialize`, `validate()` → open
  diagnostics, `prepare()` → `swmm_engine_start`, `update()` →
  `swmm_engine_step` (one routing step per update, the SWMMComponent
  precedent), `finish()` → `end+report+close`. Status events per the
  `ComponentStatus` state machine — the Composer's progress UI runs on them.
- Exchange items per HC2b's table, geometry from `swmm_spatial_*`.
- `OpenSWMMComponentInfo : IModelComponentInfo` with the factory entry the
  Composer's own plugin discovery expects.
- **Verify (G-UT5):** Composer smoke — load, instantiate on a corpus deck,
  run to completion, plot an output. Then the REAL gate: openswmm(hydraulics)
  → CSHComponent(stream temperature) on the CSH test reach, against
  openswmm's own H1–H6b heat on the same network. The two will not agree
  numerically (different discretizations); the gate is directional and
  band-limited, and the BAND IS RECORDED with its justification, not tuned
  until green (lesson 57's family).

### HC4 — coupled compositions (Direction A pays out)

1. openswmm ⇄ `GWComponent` (node Darcy exchange) — the Phase-4 G-plan's
   stated interim, giving GW-coupled results before G1/G2 land internally.
2. openswmm ⇄ `HTSComponent` — the external bed zone against H6b's internal
   one, SAME network, SAME parameters. This is the parity instrument the H6b
   round could not have: the reference implementation running live rather
   than transcribed. Divergences 1–3 (BedZoneData.hpp) predict exactly where
   they will disagree; the composition measures how much.

### HC5 — Direction B: the engine hosts (the refusal retires)

- `PluginType::PROCESS_COMPONENT` + `hydrocouple_component_info()` as a
  second dlsym target beside `openswmm_plugin_info` (PluginFactory already
  isolates symbol handling; RC.2's leak fix is the pattern).
- A loaded `IModelComponent` is driven through a `ProcessComponentBinding`
  adapter (HC2a's seam): `bind` = initialize+prepare with exchange items
  wired per HC2b, `step` = update to the engine clock, `finish` = finish.
- **Threading/exceptions rule, decided now not discovered later:** the
  component runs on the routing thread, synchronously; a component exception
  is caught at the seam and fails the RUN with the component's id in the
  message. No async updates in v1 — the Composer's pull model tolerates
  them, our fixed-step loop does not.
- The refusal message narrows for the last time: unknown ids still refuse;
  library ids load or fail loudly with the dlopen/dlerror text.
- **Verify:** host `HTSComponent` itself (the round-trip: the library the
  reference decks run under the Composer, loaded by us). Corpus untouched —
  no corpus deck names a library component, and falsifier: a deck naming a
  nonexistent library must fail at open with the path in the error.

## 3. Risks, ranked

1. **Qt.** `HydroCoupleSDK` (which SWMMComponent/HTSComponent link) is
   Qt-based; the interface headers are not. Direction A's wrapper must link
   SDK+Qt — acceptable in `src/couplers/` (the GUI repo already ships Qt),
   NEVER acceptable in `src/engine/`. If SDK types leak into the engine
   target, stop and re-scope; that is the one-way door here.
2. **Time-step negotiation.** HydroCouple components advance on their own
   adaptive clocks (`HTSComponent` MAX/MIN_TIME_STEP). Direction A: the
   Composer mediates — low risk. Direction B: our loop must call update
   until the component reaches `t + dt` — the HC2a `step` contract makes the
   component responsible, matching `ITemporalModelComponent` semantics.
3. **The 32/64-bit and compiler-ABI question for loaded C++ plugins** (B
   only). C++ plugin ABIs are fragile across compilers; the mitigation is
   the C-API-shim pattern (the component links our C API, we exchange POD),
   which HC5 should PREFER over raw C++ interface passing if any ABI issue
   surfaces in the first composition.
4. **Scope creep via the spatial interfaces.** `hydrocouplespatial.h` is
   large (polyhedral surfaces, TINs). HC2b's table needs points, polylines
   and scalars only. Anything more is Phase-3 (2D) work and waits.

## 4. Sequencing against the rest of the program

HC1 any time. HC2a/HC2b next quality lull. HC3 after the E6 API round is
validated (the wrapper consumes it). HC4.2 (HTS composition) is most valuable
soon after H6b's bindings validate, while the divergence predictions are
fresh. HC5 last, and only if a concrete deck needs hosting — Direction A may
satisfy the actual demand indefinitely, and an unused hosting ABI is exactly
the kind of speculative surface CLAUDE.md §2 forbids.
