# The snow mixing volume — a live defect across four shipped phases

**Found:** 2026-08-20, while checking prerequisites for the API bundle
(E6+R5+A6+IO5), which is the plan's next slot after H5.
**HEAD:** `0e8e57df`. **Standing findings:** lessons 1–103.
**Status:** nothing implemented. This document exists to characterise the
defect precisely and to put one decision to the user.

---

## 1. Why this was looked at before starting A6

The roadmap has carried `WATER_AGE_SNOW` as **untouched *and* undeferred**
for the whole program — no error names it, no owed row explains it. A6 is
"Python/C/MCP age surfaces": an API over the age numbers. Building an
interface over a quantity that is wrong on a class of decks is the
ship-a-defect-behind-a-nicer-façade pattern, so the gap was checked first.

**It is worse than unimplemented.** The plan's §8 describes an *enhancement*
("snowmelt inherits snowpack age; disable with `WATER_AGE_SNOW OFF`"). What
is actually in the code is a **wrong mixing volume**, and it is wrong
independently of whether a pack ever carries an age of its own.

## 2. The defect

On a subcatchment **with a snowpack** and `IGNORE_SNOWMELT` off, the runoff
solver and the transport modules disagree about how much water arrived.

**What the solver uses** (`Runoff.cpp:548-552`):

```
precip_imperv = snow_net_imperv[ui]    // IMPERV0 and IMPERV1
precip_perv   = snow_net_perv[ui]      // PERV
```

built at `SWMMEngine.cpp:1608-1616` as **`imelt + rainfall·(1 − asc)`** —
melt, plus the rain falling on the snow-free fraction. Per subarea, ft/s,
sentinel `-1.0` for "no pack" (`SubcatchData.hpp:642`).

**What every transport module reads:** `ctx.subcatches.rainfall[ui]`, which
is set once at `Runoff.cpp:294-295` as

```
precip_[ui] = rain + snow;             // snow is SNOWFALL, not melt
ctx.subcatches.rainfall[ui] = precip_[ui];
```

and **never updated afterwards**. The comment at `Runoff.cpp:324` already
calls `precip_` "rainfall + snowmelt" — it is not; that line is the first
sign of the confusion.

**Four shipped phases read it:**

| phase | site | reads |
|---|---|---|
| A3 | `WaterAgeWatershed.cpp` | `ctx.subcatches.rainfall[ui]` |
| H5a | `HeatWatershed.cpp` | same |
| A4 | `SWMMEngine.cpp:1735` → `setLidInflowAge` | same |
| H5b | `SWMMEngine.cpp:1735` → `setLidInflowTemperature` | same |

### 2.1 Two errors, in opposite directions

1. **Snowfall is counted as arriving liquid.** Water being *stored in the
   pack* is mixed into the surface as though it had landed — injecting
   rain-age, rain-temperature water into a surface that received none.
2. **Snowmelt is not counted at all.** The water that genuinely arrives is
   invisible to the mixing volume, so the surface ages as if nothing
   arrived. **That is A3's own net-gain failure mode** (lessons 64, 68)
   reappearing through a different field — the same "the mixing volume is
   not the arriving volume" error, one layer up.

They do not cancel: they are displaced in time by the whole residence of the
pack, which is the quantity a snow-aware age model exists to measure.

### 2.2 And the rate is per-subarea, while the transport modules use one

`sni` and `snp` differ — the impervious value is an area-weighted blend over
plowable and non-plowable fractions. A3 and H5a apply a single scalar to all
three subareas. Even with the right total, the split would be wrong.

### 2.3 Scope note, deliberately not overclaimed

`SWMMEngine.cpp:1757` also uses this value as the LID's **hydraulic** inflow
(`g.inflow[uu] = rain + q_from_sc`). Whether *that* is correct is a separate
question about legacy fidelity — legacy `lid.c` also reads subcatchment
rainfall — and **this document makes no claim about it.** The transport
claim stands on its own: the modules read a field whose meaning is not the
one they need.

## 3. What is a defect fix and what is a decision

**(a) The mixing volume is a defect fix and needs no decision.** When a pack
is present, the arriving-water rate is `snow_net_imperv` / `snow_net_perv`,
per subarea, exactly as the solver uses them. This is not a feature; it is
reading the field that means what the code says it means.

**(b) What the arriving melt is *worth* is the decision** — the plan's open
item ("Default for `WATER_AGE_SNOW`, proposal: ON, pack ages", §8 / §212).
Three positions, and **age and heat may want different answers**:

- **Age.** Melt that has sat in a pack for a week is a week old. Treating it
  as fresh rain understates residence time by exactly the pack's holding
  time, which for a snow study is the number of interest. But a pack age
  state is new state, with hotstart and reporting consequences.
- **Heat.** Meltwater is at **0 °C essentially by definition** — that is
  what melting means. Treating it as arriving at the configured RAINFALL
  temperature is not a small approximation; on a winter deck it is the
  difference between a stream fed by snowmelt and one fed by rain. This may
  be the stronger case of the two, and it needs no pack state at all.

**(a) without (b) is coherent:** melt would arrive at the configured RAINFALL
value, which is *a defensible number over the right volume* rather than a
defensible number over the wrong one. That is a strict improvement and it is
independently landable.

## 4. What is NOT yet known

- **No gate anywhere in the program uses a `[SNOWPACKS]` deck.** So the
  defect is unobserved, and the magnitude above is reasoned from the code,
  **not measured**. The first thing any implementation round must do is build
  a snow deck and *measure* the error — lesson 91: a claim in a document is
  not an arithmetic check.
- Whether `asc` (areal snow coverage) makes the blend continuous enough that
  a naive fix introduces a discontinuity at pack formation/disappearance.
- Whether the hotstart carries pack state (A2a deferred subarea depths for
  exactly this class of reason).
