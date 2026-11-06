# Writer round-trip parity harness

Does a saved model still mean what it meant? Nothing else in either tree asks
that. `tests/parity/run_corpus.sh` compares v6-base against v6-patched on
pristine decks; the benchmarks sweep compares legacy against v6, also on
pristine decks. Both are blind to the writer. This closes that gap.

Findings live in `plans/FILE_IO_PARITY_AUDIT_2026-09-22.md`.

## Build

```sh
./build_rt_write.sh                          # links against build/darwin
ENGINE_BUILD=/path/to/build ./build_rt_write.sh
```

`rt_write` must link the engine whose **writer** is under audit. The installed
Python bindings are not a substitute — they are a snapshot and have lagged the
source tree by two weeks.

## Run

```sh
python3 run_writer_roundtrip.py --no-run --out _sweep_g4      # G4 only, fast
python3 run_writer_roundtrip.py --workers 4 --timeout 120 --out _sweep_gated
python3 summarize.py _sweep_gated/results.jsonl
python3 summarize.py _sweep_gated/results.jsonl --section XSECTIONS
```

## The four gates

| Gate | Question | Needs |
|---|---|---|
| G1 | does legacy accept the round-tripped file? | legacy binary |
| G2 | does legacy simulate it identically? | legacy + `harness.compare` |
| G3 | does **v6** simulate it identically? | v6 + `harness.compare` |
| G4 | does the text differ from generation zero? | nothing |

**G2 is the arbiter.** Legacy normalises values itself — it clamps a negative
conduit offset to zero (`link.c:1061-1069`), raises a regulator crest to the
downstream invert (`:424-440`), forces node max depth to the link crown. Our
engine reproduces those and the writer persists them, so a faithful file still
differs textually. Never read a G4 finding as a defect without checking G2.

**Always compare against generation ZERO.** The gtest suite's strongest
assertion is `gen2 == gen3` convergence, which cannot see a value that decays to
a fixed point: `[LOSSES]` seepage written in internal units prints `0.000000`
after one save and then converges perfectly at zero forever.

## Gotchas

- Legacy **exits 0** having written `ERROR 211` and no results. G1 reads the
  `.rpt`, never the exit code.
- Legacy writes an **empty `.out`** unless the deck reports objects, which would
  make G2/G3 vacuously green. The harness forces full reporting on both sides.
- 8 workers has produced **false timeouts** on this corpus. 4 is the default;
  raise it only with a raised `--timeout`.
- Originals are frequently CRLF. `gen0.inp` is written LF-normalised once so
  later comparisons are not swamped by false differences.
- `--lenient` mirrors the mode the GUI opens in and is what the sweep uses;
  strict open rejects decks over unknown option keywords.
- **Disk.** A gated sweep writes four `.out`/`.rpt` pairs per deck. Retaining
  every one filled a 900 GB volume at deck 738 of 1396. `--keep fail` (the
  default) discards passing decks and prunes the `.out` binaries from failing
  ones, leaving both `.inp` generations and all four `.rpt` files — about 4 MB
  per kept deck instead of ~100 MB. `--keep all` needs ~25 GB.
- Each deck runs with **cwd set to its own directory**. Without that, decks that
  name report or LID output files by relative — and sometimes absolute Windows —
  paths create literal files in the CWD, and legacy leaves `swmmXXXXXX` scratch
  files behind, all of it landing in the source tree.

## Extending the differ

`inp_normalize.py` groups sections three ways: keyed on the object name
(most sections, including multi-line ones like `[CURVES]` where order matters
only *within* a name), genuinely positional (`[CONTROLS]`, `[TRANSECTS]`,
`[TITLE]` — the first token is a repeating keyword), and unknown sections, which
default to positional because over-reporting is the safe failure.

Numbers compare numerically; clock and decimal-hour forms compare as hours, so
`0.1` equals `0:06` but `0.105` does not — sub-minute loss stays visible.

Artifacts (`_sweep*`, `probes/_*`, `rt_write`) are gitignored. `probes/` holds
hand-built decks where every column carries a distinctive value, so a dropped or
shifted column is unambiguous.
