# FV node-stage steps — diagnosis, 2026-08-20

Probe: `fv_node_stage.cpp` in this directory. Question: FV profile plots show the
node stage above the adjacent conduit water surface. Which nodes, how big, and is
it a reporting artifact or a routing one?

## Build and run

```bash
cd /Users/calebbuahin/Documents/Projects/cbuahin_github/openswmm.engine
clang++ -std=c++20 -O2 -o tests/manual/fv_node_stage/fv_node_stage \
  tests/manual/fv_node_stage/fv_node_stage.cpp \
  -Iinstall/Darwin/include -Linstall/Darwin/lib -lopenswmm.engine \
  -Wl,-rpath,$PWD/install/Darwin/lib

./tests/manual/fv_node_stage/fv_node_stage <model.inp> <model.out>
```

`<model.out>` must be a completed **FV** run of that `.inp`, with `[REPORT]`
`NODES ALL` / `LINKS ALL`. The probe reports, per node class, the distribution of

```
published node head  −  highest adjacent conduit water surface at that node's end
```

Positive = the node stage sits above the pipe, i.e. the step seen in the profile.

## What the code does (the thing being tested)

`Routing.cpp:1254-1289`:

```
head = fv_state_.node_head[n];                 // solver's own head
if (node_passthrough()[n])                     // ← overridden ONLY here
    head = max over WET incident faces of min(cell_zb, face_zb) + cell_h;
depth = head − node_invert;
```

Pass-through requires **all** of: junction, no storage volume, **exactly two**
incident conduit faces, neither carrying a culvert inlet or flap gate, no
structure flow, no carry, lateral inflow zero-or-divertible
(`ExplicitFvSolver.cpp:143-163`, `:1818-1834`). Everything else publishes the raw
solver head.

## Result 1 — the step is confined to the nodes the fix does not cover

`Example1.inp` under `FLOW_ROUTING FV`, default mesh, 360 report periods:

| node class | count | % | p50 | p90 | p99 | max |
|---|---:|---:|---:|---:|---:|---:|
| passthru (fix applies) | 4 | 33.3 % | −0.0451 | −0.0005 | −0.0001 | **+0.0001** |
| **algebraic (RAW head)** | 7 | 58.3 % | −0.0000 | +0.0230 | +0.0762 | **+0.2741** |
| outfall | 1 | 8.3 % | −0.0430 | −0.0063 | −0.0016 | −0.0013 |

Read the **positive tail**, which is the symptom: a pass-through node never rises
above the adjacent conduit surface (max **+0.0001**), while the uncovered class
reaches **+0.27 ft**. (The pass-through p50 of −0.045 is the node sitting *below*
the conduit mean, which is not the reported artifact.) The mechanism is not
subtle: it lands exactly on the class boundary.

## Result 2 — coverage is a function of network branching

| model | nodes | pass-through | coverage |
|---|---:|---:|---:|
| `reach_graded_500` (pure degree-2 chain) | 501 | 499 | **99.6 %** |
| `Example1.inp` (branching network) | 12 | 4 | **33.3 %** |

On the chain the fix covers essentially everything and the step is near zero
(p50 −0.0080, p99 +0.0114). On a branching network two thirds of nodes fall
through to the raw head. **A real all-pipes model, where manholes routinely join
three or more pipes, will be worse than Example1, not better.**

## Result 3 — the residual is first-order in Δx (it is a datum artifact)

`Example1.inp`, `FV_MIN_CELLS` 1 → 2 → 4, algebraic (uncovered) class:

| FV_MIN_CELLS | p90 | p99 | max |
|---:|---:|---:|---:|
| 1 | 0.0990 | 0.1980 | 0.9098 |
| 2 | 0.0455 | 0.1350 | 0.4371 |
| 4 | 0.0230 | 0.0762 | 0.2741 |

Ratios per mesh doubling: p90 0.46, 0.51 — **clean first-order halving**. That is
the signature of the half-cell datum term S₀·Δx/2, i.e. a **reporting** term.

The pass-through class's positive tail over the same sweep is 0.0002 → 0.0001 →
0.0001: flat, and at the noise floor at every refinement. The face-consistent
reconstruction removes the Δx dependence entirely.

**Oracle check.** The probe compares against the published link depth, which is
`depthOfArea(a_mean)` — a length-weighted conduit **average**
(`Routing.cpp:1191`), not the end-cell depth. That could in principle manufacture
its own Δx-dependent offset. It does not drive this result, for two reasons: the
two classes share the same oracle and only the node-head source differs, and at
`FV_MIN_CELLS 1` the conduit has a single cell, so the oracle *is* the adjacent
cell exactly. At that refinement the uncovered class sits **+0.91 ft** above its
own adjacent cell while pass-through sits at **+0.0002** — four orders of
magnitude apart, with no averaging in the comparison at all.

The split-Riemann head loss (`ExplicitFvSolver.cpp:841-844`, `:1807-1810`) does
**not** vanish under refinement, so it is not what dominates here.

### Divergence from the earlier TwinOaks finding — stated, not smoothed over

Prior work refuted the half-cell datum as the driver of HGL steps on TwinOaks:
p99 step *rose* 3.85 → 4.98 → 6.29 ft over `MIN_CELLS` 1 → 2 → 4. Example1 does
the opposite, halving cleanly. The models differ in kind — TwinOaks surcharges and
pressurizes, Example1 is open-channel throughout — so both can be true, with a
second, pressurization-linked mechanism dominating on TwinOaks (the slot ∝ c²
amplifier was separately confirmed there). **This diagnosis therefore covers the
open-channel case only.** A surcharged model needs its own run of this probe
before the conclusion below is applied to it.

## Result 4 — after the fix

The reconstruction now covers storage-less clean junctions at **any** degree
(`Routing.cpp`, gated on `ExplicitFvSolver::node_publish_stage()`).
`Example1.inp`, `FV_MIN_CELLS 1`, where the oracle is exact:

| class | max BEFORE | max AFTER |
|---|---:|---:|
| algebraic, no lat inflow | +0.2796 | **+0.0002** |
| algebraic, has lat inflow | +0.9098 | **−0.0001** |

Routing is untouched: continuity identical (−0.111 % / −0.000 %) and the Link
Flow Summary is byte-identical before and after. Full FV suite 6/6.

At `FV_MIN_CELLS 4` a residual remains (max +0.16 on the no-lateral class).
That is most likely the oracle rather than the engine — the published link depth
is a conduit average, so at 4 cells it is no longer the end-cell depth — but it
is **not proven**, which is why the `MIN_CELLS 1` row above is the trustworthy
one.

### Three bugs it took to get there, all caught by measurement

1. **The first attempt did nothing at all** — numbers identical to eight
   decimals. The gate had been copied from the pass-through test, including
   `node_carry_ == 0.0`. A *solved* algebraic junction banks a root-solve
   residual every substep by construction, so that condition is false for
   essentially every node in the target class and excluded exactly the nodes
   the change existed for.
2. **The second attempt was a regression.** J11's reported average depth went
   0.28 → **4.11 ft**. Conduit C2 enters J11 4 ft above its invert, and an
   unguarded `max` over incident cells read that perched pipe's stage as node
   depth. A face perched above the node's own surface is a free overfall and
   must not vote. With the guard, J11 reads 0.14 / 2.28 — *below* its original
   0.28 / 2.54, which is the bias being removed rather than added.
3. **`FvEngine.PondedNodeStillReportsItsFloodingRate` failed.** A pondable
   junction above its rim is *dynamically* demoted to the bucket path
   (`algebraicActive`), where its head comes from a real volume ledger the
   incident cells cannot describe. Gating on the static classification erased
   the pond. Eligibility is now re-evaluated against the state actually reached
   at the end of `advance()`.

## Conclusion

For open-channel FV models the elevated node stage is a **reporting artifact**,
confined to nodes outside the pass-through class, scaling as O(Δx), and already
solved for the one class that receives the face-consistent reconstruction.

**Done: the `Routing.cpp` reconstruction now covers storage-less clean junctions
at any degree** (§Result 4), with a perched-face guard and the ponding demotion
honoured.

This stays **publish-side**, which the safety constraint requires: solver-internal
heads double as ghost boundary states under LTS tier holds, and changing the
in-solver estimator previously turned a 0.007 cfs lake-at-rest residual into a
70.9 cfs standing oscillation.

**Not resolved by this work:** whether bucket nodes at `MIN_SURFAREA` (pump-only
wet wells, ponding-demoted junctions) and surcharged models carry a separate
mechanism. Neither appears in Example1.

## Files

- `fv_node_stage.cpp` — the probe
- `runs/graded500_rep.{inp,rpt,out}` — degree-2 control (reporting enabled;
  the benchmark decks ship `NODES NONE` and cannot be used directly)
- `runs/e1_mc{1,2,4}.{inp,rpt,out}` — the refinement sweep
