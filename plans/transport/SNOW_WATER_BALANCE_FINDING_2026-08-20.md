# The snowpack water balance — four divergences from legacy

**Found:** 2026-08-20, checking S2a §6's open question before letting S2b's
age model rest on it.
**HEAD:** `8b7d1cf7`. **Standing findings:** lessons 1–111.
**Status:** nothing implemented. **This changes hydrology on every snow deck,
so it is a decision, not a fix I should take.**

---

## 1. Why this was checked

S2b's age model is a **complete-mix over the pack's water**. An age model
built on a balance with holes inherits them, and shipping that as an
"approximation" is lesson 64 exactly. S2a's §6 raised one question as a
*question*; checking it against legacy turned up four things, and three of
them are in code S2a's round did not look at.

## 2. Legacy is the reference, and it is compact

`src/legacy/engine/snow.c`, `routeSnowmelt`:

```c
vmelt = smelt * tStep;
vmelt = MIN(vmelt, snowpack->wsnow[i]);
snowpack->wsnow[i] -= vmelt;                          // (A) SWE −= MELT
snowpack->fw[i] += vmelt + rainfall * tStep * asc;    // (B) rain-on-snow in
vmelt = snowpack->fw[i] - Snowmelt[k].fwfrac[i] * snowpack->wsnow[i];
                                                      // (C) cap on POST-melt SWE
vmelt = MAX(vmelt, 0.0);
snowpack->fw[i] -= vmelt;
return vmelt / tStep;                                 // what leaves the pack
```

`src/engine/hydrology/Snow.cpp` steps 6–7 restructure this, and the
bookkeeping diverges three ways.

## 3. The divergences

### 3.1 Rain falling on the snow-covered fraction is DISCARDED

Legacy (B) adds `rainfall · dt · asc` to the pack's free water. **The engine
never adds it** (`Snow.cpp:314` is `fw += imelt·dt` alone).

That water is also excluded from what reaches the ground — `snow_net` carries
`rain·(1 − asc)` by construction. So it **neither enters the pack nor reaches
the surface. It vanishes.**

Magnitude: everything falling as rain on a snow-covered area. Under the ADC
default of full cover (lesson 110) that is *all* the rain on a snowy
subcatchment.

### 3.2 SWE is reduced by the DRAINED EXCESS, not by the melt

Legacy (A) reduces `wsnow` by `vmelt`, the melt, **before** the free-water
bookkeeping. The engine reduces it at step 7 by `imelt` — which step 6 has
just **overwritten with the excess that drained out**.

So snow that melted but is retained as free water within capacity is still
counted in `wsnow`. **It exists twice: as snow and as free water.** A pack
whose melt never exceeds its free-water capacity never depletes at all.

This is the one I would rank highest: it is mass creation, and it makes a
pack persist indefinitely under slow melt.

### 3.3 The free-water capacity is computed from PRE-melt SWE

Legacy (C) applies `fwfrac` to the **post**-melt `wsnow`; the engine's
`fw_cap` at `Snow.cpp:313` uses the pre-melt value. Smaller than the other
two, but it shifts the drainage threshold every step and compounds with 3.2.

### 3.4 The instant-melt branch discards its water (found by the S2a round)

`Snow.cpp:205`: the sub-0.001-inch branch does `imelt += (ws + fw)/dt`, then
**step 4 assigns `imelt` unconditionally** and step 5 zeroes it because
`wsnow` is now 0. The water from an instantly-melted thin pack is discarded.
Small volumes; same balance.

## 4. Why none of this has ever been visible

**`setMeltCoeffs` had no caller until `274b6506`** (lesson 104), so
degree-day melt was identically zero and `imelt` only ever came from
rain-on-snow above 0.02 in/hr. With no melt, steps 6–7 had nothing to
mis-account. **These four became reachable one commit ago.**

That is also why this is not a regression: it has always been there, and the
program only just built the first deck that can see it.

## 5. What is NOT established

- **No measurement.** The magnitudes above are read from the code, not run —
  lesson 91. A fix round must measure each on a snow deck before and after.
- Whether any of the 14 reference decks contains `[SNOWPACKS]`. **If one
  does, fixing this moves the bit-identity corpus**, and that needs to be
  seen before it is done.
- Whether 3.3 alone changes anything observable, or is absorbed by 3.2.

## 6. The decision

Fixing this is a **hydrology change that moves runoff volume and timing on
every snow deck**. It is not a transport change and it is outside what S1/S2
were scoped to do. Three positions:

1. **Fix the balance to match legacy, then S2b.** The age model then rests on
   a sound balance. Cost: a hydrology round with its own deck-level
   validation, and any snow deck's results move.
2. **S2b first, on the current balance, with the divergences recorded.** The
   age numbers would be consistent with the engine's own hydrology, which is
   what a user comparing age against runoff would expect — but that hydrology
   creates and destroys water.
3. **Fix only 3.1 and 3.4** (the two that lose water outright) and leave 3.2
   and 3.3, which are ordering rather than loss. Smaller blast radius, but
   3.2 is the mass-creation one, so this fixes the smaller half.

I recommend **(1)**, and the same reasoning applies as two rounds ago: an API
over wrong numbers, or an age model over an unsound balance, is the
ship-a-defect-behind-a-façade shape. But the cost lands on hydrology results,
which is the user's call and not mine.
