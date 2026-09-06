# Dry-Link Hotstart Gate — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent.
**Base:** `584d1065` (post dry-age mask) — or current HEAD; the change is
test-only and independent of the multicolumn-series work that has been
landing alongside.
**Not a plan phase** — the gate the dry-mask validation §6.4 recorded as
owed: falsifier iv (mask the STATE instead of the report) escapes all 141
tests, so the state/report separation is a design claim with no observer.

---

## 1. What is unobserved

`584d1065` masks the REPORTED age of a dry element to 0 while leaving the
aged value in `water_age_state`. Validation established that masking the
STATE instead passes the entire suite — 140/141, identical to clean.

Its own §6.4 was blunt about the consequence, and I accept the correction:
**the "a refilling pipe would jump" half of my rationale does not
survive** — the stale state occupies 0.0107 ft³ against 1263 ft³ arriving,
an influence of order 1e-5. Exactly one consequence of the separation is
real: **hotstart fidelity**. A save taken while an element is dry must
carry the aged value forward, because the restart may refill it.

So the gate is the one the validator specified, in the file it specified.

## 2. Changeset (uncommitted — TEST-ONLY)

```
mod:  tests/unit/engine/test_water_age.cpp
      (write_deck gains a `dry` knob — InitDepth 0 everywhere, FREE
       outfall, no [INFLOWS]; +1 gate
       DryElementHotstartCarriesTheAgedState)
```

No production code changes. TU passes `g++ -std=c++20 -fsyntax-only`.

## 3. The gate

Bone-dry deck, `INITIAL_STATE 6 h`, one hour of simulation, then
`swmm_hotstart_save`:

1. **The discriminator:** the SAVED link age must exceed 21600 s (the 6 h
   seed) — the state kept aging while every element was dry, which is
   what the separation asserts. Masking the state instead of the report
   makes this read 0.
2. **Round trip:** reload into a fresh engine (open → initialize → apply,
   the lifecycle A2a validation established) and compare bitwise, as A2a's
   own hotstart gate does.

## 4. Validation protocol

1. Build; `ctest -R test_engine_water_age` — 17 gates (16 + 1).
   *Anticipated failure modes, likelihood order:*
   (a) **The dry deck may not stay dry** — unlike the mask gates in
   `test_output_quality.cpp`, this one has **no liveness assertion on
   wetness**, because what it needs is the STATE, not the depth. If you
   want it airtight, add one: assert the reported/link volume is below
   `ZERO_VOLUME` at some step, mirroring repair 1 from the mask round.
   I judged the 21600 s discriminator sufficient (a wet deck would still
   age, so the gate would still pass — meaning a wet deck makes it
   WEAKER, not wrong). Your call; recording the reasoning either way is
   the point.
   (b) **`link_age` may be 0 for a different reason** — if ARD link-cell
   seeding turned out to be wetness-gated (node stores are; §6.4 notes
   link cells are NOT), the gate would fail on arrival for a reason that
   is not the mask. That would itself be worth knowing: it would mean dry
   links never carried the aged state at all, and the mask was papering
   over a seeding asymmetry.
   (c) Reload ordering is open → initialize → apply → (no start needed;
   the gate reads state directly).
2. **Falsifier sweep — this gate exists for exactly one:**

   | falsifier | expected |
   |---|---|
   | iv (the reason for this gate). Mask the STATE instead of the report: zero `water_age_state` for dry elements in the ARD publish/step path | **must FAIL** — saved link age reads 0 instead of ~25200 s. If it still passes, the gate does not observe what it claims and needs rework, not acceptance |
   | i. remove the report-side link mask | must stay GREEN — this gate is about state, and the mask gates in `test_output_quality.cpp` own the report side. A failure here would mean the two concerns are entangled |
3. **Prior suites:** everything unchanged (test-only, one new gate). The
   water-age suite must be 17/17 and `test_engine_output_quality` 8/8.
4. **Record:** whether falsifier iv now fails. That single result is the
   entire value of this changeset — if it does not, say so plainly and
   the separation remains unobserved.

## 5. Commit message

```
test(transport): observe the water-age state/report separation

The dry-element mask (584d1065) leaves the aged value in water_age_state
while reporting 0, and its validation established that masking the state
instead passes the whole suite - 140/141, identical to clean. The
separation had no observer.

Only one consequence of it is real. The "a refilling pipe would jump"
argument does not survive scrutiny: the stale state occupies 0.0107 ft3
against 1263 ft3 arriving, an influence of order 1e-5. What does hold is
hotstart fidelity - a save taken while an element is dry must carry the
aged value forward, because the restart may refill it.

This gate runs a bone-dry deck (InitDepth 0, FREE outfall, no inflows)
with INITIAL_STATE 6 h, saves a native hotstart, and asserts the SAVED
link age exceeds the 6 h seed - the state aged while every element was
dry - then round-trips it bitwise through a reload. Masking the state
instead of the report reads 0 here and fails.

Test-only; write_deck gains a `dry` knob.
```

## 6. Validation results

*(appended by the checking agent)*
