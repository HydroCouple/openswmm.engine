# FV phase split: the parts exceeded the whole — 2026-08-21

The `[PERF-FV]` phase table exists to rank optimization targets. It could not,
because `total` (the sum of the phases) came out **larger than `step`** (the
routing time it is a breakdown of) on every run with local time stepping on.
`unattributed = step - total` is not a quantity that can be negative.

## Cause

`perf::GatedTimer` accumulated **wall** time, so a timed phase that calls
another timed phase booked its child's time twice — once in the child's
accumulator and once inside its own. Three such nestings exist:

| outer | inner | path |
|---|---|---|
| `settleAccumulators` | `refreshDepths` (×2) | LTS only |
| `restoreState` | `refreshDepths` | rollbacks, both paths |
| `runMacroCycle`'s settle window | `refreshFvBoundaryFlows` (`bndcallback`) | LTS only |

Two of the three are LTS-only, which is exactly why only the LTS rows went
negative.

## Fix

`GatedTimer` now books **self** time: a thread-local tally (`perf::nested_wall`)
collects the wall time of timers that open and close inside the running one, and
each timer subtracts it before accumulating. The phases now partition time
instead of overlapping it. No call site changed, nothing is atomic, and with
`OPENSWMM_PERF` unset the destructor still returns before touching anything.

## Measurement

Same binary, same decks, `OPENSWMM_PERF=1`, seconds. `e1_mc4` is Example1 at
`FV_MIN_CELLS 4`; `graded500_rep` is the 500-conduit graded chain.

| deck | build | step | total | unattr % | refreshdepths | settle | restore |
|---|---|---:|---:|---:|---:|---:|---:|
| e1_mc4 LTS | before | 2.683 | 3.118 | **−16.2** | 0.839 | 0.571 | 0.000 |
| e1_mc4 LTS | after | 2.498 | 2.380 | **+4.7** | 0.765 | 0.004 | 0.000 |
| e1_mc4 no-LTS | before | 2.661 | 2.522 | +5.2 | 0.310 | 0.001 | 0.000 |
| e1_mc4 no-LTS | after | 1.845 | 1.731 | +6.2 | 0.255 | 0.001 | 0.000 |
| graded500 LTS | before | 10.409 | 12.386 | **−19.0** | 3.379 | 2.255 | 0.089 |
| graded500 LTS | after | 9.247 | 8.928 | **+3.5** | 3.077 | 0.007 | 0.000 |
| graded500 no-LTS | before | 7.839 | 7.592 | +3.2 | 1.114 | 0.000 | 0.099 |
| graded500 no-LTS | after | 6.732 | 6.413 | +4.7 | 1.033 | 0.001 | 0.001 |

(`step` differs run to run; it is machine noise, and `unattr %` is a ratio taken
within a single run, so the sign flip does not depend on it.)

## What the old table was telling people to do

`settle` was **2.255 s on graded500 LTS — 18 % of the reported total — and its
own work is 0.007 s.** Effectively all of it was the two `refreshDepths()` calls
inside it, already counted under `refreshdepths`. `restore` is the same story:
0.089 → 0.000 and 0.099 → 0.001.

Anyone ranking targets off the old table would have gone after
`settleAccumulators`, which has almost nothing in it. The real target is
`refreshDepths` — 33 % of `total` on graded500 LTS and 32 % on Example1 — and
that was legible only after the double count was removed.

## Reproduce

```bash
cmake --build build/darwin --target openswmm -j 8
D=tests/manual/fv_phase_timers/runs
for f in $D/*.inp; do
  b=$(basename $f .inp)
  OPENSWMM_PERF=1 build/darwin/src/cli/openswmm $f $D/$b.rpt $D/$b.out 2> $D/$b.perf
done
grep -ho '\[PERF-FV\].*' $D/*.perf
```

The decks are copies of `tests/manual/fv_node_stage/runs/e1_mc4.inp` (Example1
under `FLOW_ROUTING FV` at `FV_MIN_CELLS 4`) and `graded500_rep.inp`; the
`_nolts` variants differ only by `FV_LTS NO`. Like that directory, `runs/` is
left untracked — the decks and their output are reproducible and are here to be
read, not versioned.

The `.before.*` files are the same decks run against the `2a5b964f` timer, kept
as the falsifier: if `GatedTimer` ever books wall time again, `unattr %` goes
negative on the LTS rows and nowhere else.
