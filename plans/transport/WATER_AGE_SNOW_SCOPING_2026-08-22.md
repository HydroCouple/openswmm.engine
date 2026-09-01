# `WATER_AGE_SNOW` — scoping (2026-08-22)

**Question carried since A3 as "untouched AND undeferred"** — the phrase
matters, because undeferred means no error names it, so a deck writing
`WATER_AGE_SNOW OFF` today gets silence rather than a refusal.

**Short answer: S2b answered the behavioural half and nobody noticed, and what
is left is a smaller, different question than the one on the list.**

---

## 1. What the question actually was

`WATER_AGE_TRACKING_PLAN` §8: *"snow is held; default can be disabled via
`WATER_AGE_SNOW OFF` to treat melt as fresh"*, with §Open proposing **ON (pack
ages)**. A3's scoping put it precisely: **does the pack hold age, or merely
pass it through?**

At A3 and A4 melt reached runoff through `snow_net_imperv/perv` and was
therefore **treated as rain-aged** — pass-through, the OFF behaviour, by
omission rather than by choice.

## 2. S2b settled the behaviour, unconditionally

S2b (`2a58d82c`) gave each snow surface a water age, complete-mixed over
`wsnow + fw`, with melt leaving at the pack's age and `arrivingPrecipAge`
blending it against the rain age. **The pack holds age.** That is the proposed
default, implemented — and implemented with **no way to turn it off**.

So the behavioural question is closed. It closed in a round whose handoff
listed `WATER_AGE_SNOW` as still owed, which is worth noting on its own: a
question can be answered by a round that did not know it was answering it.

## 3. What is left, and it is two things

### 3.1 Does the OFF switch need to exist at all? — a decision, not a gap

The plan proposed the option so a user could treat melt as fresh. **Nothing has
ever asked for it**, and the case against is now stronger than when it was
proposed: pass-through is not a modelling choice a user makes, it is the
behaviour the model had while the pack's residence time was unmeasured. S1's
finding is the argument — the displacement is *the whole residence time of the
pack*, which is precisely the quantity a snow-aware age model exists to report.

**Recommendation: do not add the switch.** Record it as a plan item retired by
S2b rather than an unimplemented feature. If it is ever wanted, the
implementation is one guard in `arrivingPrecipAge` returning `a_rain`, which is
cheap to add later and impossible to remove once decks depend on it.

**Cost of being wrong:** a user who wants melt treated as fresh has no lever.
That is a one-line change to add, and no deck in any corpus wants it.

### 3.2 The deferral-discipline gap is real and is NOT closed by S2b

A3's handoff named this exactly: *"no error names it. That is a gap in the
deferral discipline, not just in features."* A deck containing
`WATER_AGE_SNOW OFF` today is **silently ignored** — the token is not
recognised, and nothing tells the user their instruction did nothing.

**This survives whichever way §3.1 goes**, and it is the part worth doing:

- if the switch is not added, the option must **refuse with a precise error**
  naming S2b and saying the pack always holds age;
- if it is added, it must be parsed.

Either way, silence is the one outcome that is wrong, and it is today's
behaviour.

## 4. Recommended disposition

| item | disposition |
|---|---|
| *Does the pack hold age?* | **CLOSED by S2b** — yes, unconditionally |
| `WATER_AGE_SNOW OFF` switch | **Retire from the plan.** Not a gap; a proposal S2b's default supersedes |
| The undeferred token | **⬜ OWED, small.** Recognise `WATER_AGE_SNOW` in the water-age component's reader and refuse it with a message naming S2b. Rides with any water-age round |

## 5. What this does not settle

- **A2** — whether the pack's *initial* water takes the `INITIAL_STATE` age —
  is a different question and was decided separately (user, 2026-08-22: yes).
  It is about seeding, not about whether the pack holds age.
- **Two stores vs one** (register §2.2 A1) is likewise separate: an
  approximation inside the ON behaviour, not a question about whether ON is
  right.
