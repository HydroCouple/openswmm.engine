# Species-ID Reader — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent.
**Base:** `d4889329` (post-A2b).
**Not a plan phase** — carry (a) from A2b validation, promoted because it
gates whether water age is USABLE, not merely reported.
**Standing findings:** lessons 1–42. This changeset exists because of
lesson 40: A2b made the species NAME the sole carrier of a column's
meaning, and no reader could read a name.

---

## 1. The gap

`DefaultOutputPlugin::writeHeader` has always emitted **four** ID lists —
subcatchments, nodes, links, **species** — but `OutputReader::readIDs`
read three and stopped. No public entry point exposed species names:
`swmm_output_*` had `..._get_{subcatch,node,link}_id` and no pollutant
equivalent, and Python exposes only `pollutant_count`.

Why that blocks A2b: the `.out` per-column unit field is a three-value
concentration enum with no HOURS slot, so the age column necessarily
reuses a concentration code and the NAME is the only discriminator. A
consumer therefore saw two columns, one of them hours labelled MG/L, with
no way to tell which. The legacy library's
`SMO_getElementName(…SMO_pollut…)` proves the list is there — nothing in
the modern stack reached it.

## 2. Changeset (uncommitted)

```
mod:  src/engine/output/OutputReader.{hpp,cpp}
      (pollut_ids_ member; readIDs() reads the FOURTH list the writer
       already emits; pollut_id(index) accessor, nullptr out of range)
mod:  src/engine/output/openswmm_output_impl.cpp
      (swmm_output_get_pollut_id — thin forward, nullptr on null handle,
       mirroring the three existing *_get_*_id functions exactly)
mod:  include/openswmm/engine/openswmm_output.h
      (public declaration + why-the-name-matters doc)
mod:  tests/unit/engine/test_output_quality.cpp   (+2 gates)
```

All touched TUs pass `g++ -std=c++20 -fsyntax-only`.

**Additive only.** No existing signature, struct layout, or file format
changes; the reader consumes bytes that were already written. Older `.out`
files parse identically (the list has always been present).

## 3. Design notes

1. **`n_polluts_` is set before `readIDs()` runs** — verified: `open()`
   calls `readFooter()` → `readHeader()` (which sets `n_polluts_` at
   line ~343) → `readIDs()`. If that order ever changes, the fourth list
   silently reads zero entries; the new gates would catch it.
2. **A malformed/truncated species list now fails the open** rather than
   being ignored, because `readIDs()` returns false. That is the correct
   strictness (the three existing lists behave the same way) but it is a
   BEHAVIOUR CHANGE for any file whose species list is corrupt — such a
   file previously opened and silently lacked names. Flag if you want a
   lenient path instead; I judged consistency with the sibling lists
   better than a special case.
3. **No Python/MCP surface in this shot.** The C entry point is the
   prerequisite; binding it is a separate, mechanical change and belongs
   with the other binding work rather than smuggled in here.

## 4. Validation protocol

1. Reconfigure if needed, build, zero new warnings.
2. `ctest -R test_engine_output_quality` — 5 gates now (3 existing + 2
   new: `SpeciesIdsAreReadableAndOrdered`,
   `SpeciesIdsReadableWithoutWaterAge`).
   *Anticipated failure modes:*
   (a) **`readIDList`'s 1024-byte sanity cap** — species names are short,
   so this should not bite; if the open now fails on an existing file,
   check whether the species list position matches the writer's (a
   mismatch means my assumption about list ORDER in the file is wrong,
   which is the one thing that would make this changeset wrong).
   (b) **Older `.out` fixtures in the repo** — if any checked-in `.out`
   was produced by a writer that did NOT emit the species list, its open
   now fails. That would be important to know; my read of the writer says
   the list has always been emitted, but a fixture is the empirical test.
   Please run the suites that open checked-in `.out` files.
3. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. remove the `readIDList(n_polluts_, …)` call | both new gates (nullptr) |
   | ii. `pollut_id` returns `pollut_ids_[index]` without the range check | out-of-range legs (2 / −1) — likely a crash or garbage under ASan; record which |
   | iii. read the species list BEFORE the link list | order gate (names swap or garble) — pins my assumption about the file's list order |
   | iv. **swap only the header NAME order** (the A2b blind spot: reorder `reported_species_names` in `SWMMEngine.cpp` while leaving the data fill alone) | `SpeciesIdsAreReadableAndOrdered` MUST fail while every value assertion still passes — this is the falsifier that was blind before, and it is the reason this gate exists |
4. **Prior suites all green** — the reader change is additive, so expect
   14/14 deck `.out` bit-identity (the writer is untouched) and every
   output-reading suite unchanged. Sanitizers over the new gates
   (out-of-range legs are ASan bait by design).
5. **Consumer follow-through (record, do not implement):** with names now
   reachable, the GeoPackage species-row gap (A2b carry (b) — nothing
   registers species names as variables, so NO species rows are emitted,
   pollutants included) becomes fixable. Note whether `pollut_id` is what
   that plugin needs.
6. Append results to §5; commit with §6.

## 5. Notes for the roadmap after this lands

Remaining A2b carries, unchanged by this shot: (b) GeoPackage emits no
species rows at all; (c) a dry element keeps aging (6.000000 h on water
that never existed — needs a wet-mask or a documented convention);
(d) pre-existing `HotStartManager.cpp:246` misaligned CRC load;
(e) ARD reads TSS 0 at an outfall where LEGACY reads 42 (pre-existing ARD
outfall behaviour); plus the owed manual note on the legitimate
LEGACY-vs-ARD link-age definition difference.

## 6. Commit message

```
feat(output): expose species IDs via swmm_output_get_pollut_id

The .out header has always carried a fourth ID list after the link IDs -
one name per species column - but OutputReader stopped after three and no
public entry point exposed it. Consumers could read pollut_count and the
per-column unit code, and nothing else.

That gap made A2b's water-age column unidentifiable: the .out unit field
is a three-value concentration enum with no HOURS slot, so the age column
necessarily reuses a concentration code and its NAME is the only
discriminator. A consumer saw two columns, one of them hours labelled
MG/L. The legacy output library's SMO_getElementName(...SMO_pollut...)
reads this same list, so the data was there all along.

OutputReader now parses the species list and exposes pollut_id(index);
swmm_output_get_pollut_id forwards it, mirroring the three existing
*_get_*_id functions. Purely additive: no signature, layout, or format
change - the reader consumes bytes already written, so existing files
parse unchanged.

One behaviour change: a truncated or malformed species list now fails the
open (readIDs returns false), consistent with the three sibling lists,
where before such a file opened and silently lacked names.

Gates: tests/unit/engine/test_output_quality.cpp +2 - names readable and
IN THE DATA'S ORDER (the assertion A2b lacked: reordering only the header
name list left every value assertion passing, and the name is exactly
what consumers must trust), plus out-of-range nullptr and a no-water-age
deck.
```

## 7. Validation results

**Verdict: accepted as written, all four falsifiers caught, no gate
strengthening needed.** Commit `06580dd6` — the FIVE files of §2 only.
A Python surface for this API is present in the working tree but is NOT in
the commit; see §7.5, which needs a decision. Artifacts:
`tests/output/species_id_validation_2026-08-17/`.

### 7.1 How this was validated

Measured in a worktree detached at `d4889329` carrying only the five §2
files, because the shared tree again held another agent's in-progress
rain-series/rename work (39 files). That isolation earned its keep twice
here: the shared tree's `.out` fixtures are currently damaged (§7.6), while
the worktree's are at their committed state — so the sweep below is a clean
measurement rather than a reading of somebody's failed run.

| check | result |
|---|---|
| configure + build | clean; **zero** warnings in the five files |
| `test_engine_output_quality` | **5/5** (3 prior + 2 new) |
| full `ctest` | **140/141** — the one failure is the known pre-existing `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph` |
| 14-deck `.out` bit-identity | **14/14** vs the A2b baseline (writer untouched, confirmed rather than assumed) |
| ASan + UBSan over the gates | **0 findings**, 5/5 |
| falsifiers | **4 of 4 caught** |

### 7.2 The behaviour change of §3.2, measured on the whole corpus

This is the item worth the most attention, so it was swept rather than spot
checked: **all 19 checked-in `.out` files in the repo were opened by both
the base and the new reader** (`sid_sweep.cpp`, `sweep_new.log`).

- **0 open-failures on either build.** The species list is genuinely present
  in every one — legacy 5.3-era (`tests/unit/legacy/output/data/Example1.out`
  → `[TSS, Lead]`), the Python solver fixtures, the hotstart set, and the
  modern files. §2's "the list has always been emitted" is confirmed
  empirically, across both writers, not just by reading the writer.
- The four parity baselines have `npollut = 0` and exercise
  `readIDList(0, …)`, which returns true with an empty list. No special case
  needed.
- **The stricter open is real, and it is an improvement.** Two files were
  crafted from `site_drainage_model.out` with the footer left intact and only
  the first species-name length corrupted — once past the 1024 sanity cap,
  once negative. The **base** reader opens both happily and reports
  `pollut_count = 1` on a file whose species block is garbage; the **new**
  reader refuses both. Turning a silently-misparsed file into a failed open
  is the safer direction, and it costs nothing on the existing corpus.

### 7.3 Falsifier sweep — 4 of 4, and iii pins the ORDER assumption

| falsifier | outcome |
|---|---|
| i. never read the fourth list | **caught** — both new gates, "species 0 name unreadable" |
| ii. `pollut_id` without its range check | **caught** — and it is a real memory error: **heap-buffer-overflow** under ASan (exit 134), so the range check is load-bearing, not decoration |
| iii. species list read BEFORE the link list | **caught** — species 0 reads `"C1"`, a LINK name |
| iv. swap only the header NAME order (the A2b blind spot) | **caught** — `SpeciesIdsAreReadableAndOrdered` fails on "species 0 is `__WATER_AGE__`" while `SpeciesIdsReadableWithoutWaterAge` correctly still passes |

Falsifier iii deserves a note: §4.3 calls it "pins my assumption about the
file's list order", and it does exactly that. Reading the species list one
position early yields conduit names, which is positive evidence that the
species list follows the links — the single assumption that would have made
this changeset wrong. It is confirmed by observation, not by reading the
writer.

Falsifier iv now fails **two independent gates**: the new reader-based
ordering assertion, and the byte-reading `read_species_ids` helper added
during A2b validation. Those overlap but are not redundant — the byte helper
checks what is IN THE FILE, the new gate checks what the READER RETURNS, and
an off-by-one in `readIDList` would separate them. Recommend keeping both.

### 7.4 §4.5 consumer follow-through: `pollut_id` is NOT what GeoPackage needs

Recorded, not implemented. The GeoPackage species-row gap is on the
in-process snapshot path, not the `.out` path: `GeoPackageOutputPlugin`
already has the names (`snapshot.pollut_names`, correct since A2b). It emits
nothing because `populate_default_variables` (`GeoPackageSchema.cpp:1330`)
inserts a fixed variable list containing no species, nothing else inserts
one, and `update()` skips any row whose `lookup_variable(name, …)` returns
−1. The fix is a variables-table registration loop over the species names;
`swmm_output_get_pollut_id` is a *reader* API and plays no part in it.

### 7.5 A Python surface for this API exists in the tree and is NOT committed

§3.3 says "No Python/MCP surface in this shot", but the working tree
contains a complete and coherent one: `swmm_output_get_pollut_id` in
`_common.pxd`, a `pollutant_ids` property plus `_read_pollutant_ids` in
`_output_reader.pyx`, and the `.pyi` stub. Either §3.3 is stale or the
binding was written ahead.

I committed the five §2 files only, for two reasons: it is the vetted scope
and the §6 message describes it, and I did not build or exercise the Python
extension, so committing it would ship code I had not validated. **Two things
to know:**

1. `python/openswmm/engine/_common.pxd` is a file **shared with the other
   agent's changeset** (it also carries their aquifer/snowpack/street/inlet/
   LID renames and gage file-column externs). Whoever commits that work next
   will carry the `swmm_output_get_pollut_id` extern along with it — while
   `_output_reader.pyx` and the `.pyi` may or may not go at the same time.
   That is how a half-bound surface happens.
2. The binding is worth landing; it just wants its own shot with the
   extension built and a Python-level gate.

### 7.6 Unrelated fixture damage already in the shared tree — NOT this changeset

`python/tests/data/solver/site_drainage_example.out` is currently **863
bytes against 56063 at HEAD**, and its `.rpt` has lost 2231 lines. Both were
rewritten at 08:22:09. The header is intact and structurally identical
(same magic, version 53000, `nsub=7 nnode=12 nlink=11 npollut=1`, same
section offsets) — only the period count differs: **60 at HEAD, 0 now**. So
some run wrote the header and no reporting periods over a checked-in
fixture. `non_existent_input_file.rpt` was likewise rewritten at 08:10:38,
and its nonconverging-node list lost an entry (`Node O1 (29.21%)`), which is
a numerical difference, not a timestamp.

This changeset cannot be the cause — it touches a *reader* and cannot affect
what a writer emits — and the validation worktree used the committed
fixtures throughout. I left the damaged files exactly as found rather than
reverting work that was not mine.

**Update, 08:29:** the damage was transient. Another re-run in the shared
tree restored `site_drainage_example.out` to 56063 bytes and undid the
`.rpt` gutting; only small timestamp-scale `.rpt` differences remain across
8 files. Nothing needs restoring now. It is recorded because it happened,
and because it is a live hazard of the shared tree: a run there can
transiently clobber checked-in `.out`/`.rpt` fixtures, and anyone who staged
broadly during that window would have committed a 0-period fixture. It is
also the concrete reason the isolated worktree is worth its cost — the
19-file sweep in §7.2 read committed fixtures and was unaffected.
