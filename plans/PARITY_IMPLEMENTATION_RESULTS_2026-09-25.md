# Parity handoff implementation — 2026-09-25

Base engine: `7de66688` (`swmm6_rel`). Base benchmarks: `0bb19c71`.
Legacy source is unchanged. No push has been performed.

## Implemented

1. **Kinematic-wave mixing inflow.** Legacy `qualrout.c:315-318` uses
   `abs(Conduit.q1) * barrels`; v6 used downstream `Link.flow`. Publish the
   accepted upstream flow from `KWSolver::execute` into `LinkData::kw_inflow`
   and consume it in the shared conduit-mixing helper. This is a parallel SoA
   vector with resize, grow, reserve, erase, reset and shrink lifecycle handling.
   It is hydraulic-step scratch, recomputed before transport after a hotstart,
   so no hotstart serialization is needed. Water age, heat and MSX use the same
   corrected mixing helper.
2. **Washoff buildup cap.** Mirror `landuse_getWashoffLoad`, legacy
   `landuse.c:578-593`: a modeled buildup function determines whether available
   buildup limits washoff. Previously RATING without buildup was erased, while
   EMC with buildup bypassed the cap. The regression exercises both sides.
3. **Storage loss reporting.** Mirror `removeStorageLosses`, legacy
   `routing.c:927-951`: book storage evaporation and exfiltration separately.
   The combined `nodes.losses` rate still drives the unchanged step flow ledger.
   The aquifer regression now verifies actual seepage and zero evaporation
   instead of codifying the former mislabel.
4. **Control action subtype validation.** Mirror legacy `addAction`,
   `controls.c:1435-1461`: a named action object must have its declared link
   subtype. Generic LINK actions remain valid. A matrix test covers every
   subtype pair; the validation API fixture now creates a pump using its named
   public enum rather than the outlet code 4.
5. **Seventeen expected oracle rejections.** Benchmark metadata names each
   authored shape and its observed legacy error. `expected_failures.report_error`
   matches a diagnostic on the rejecting engine; the run and missing-output
   comparison become XFAIL. Missing binaries, signal exits/timeouts and unrelated
   errors are not excused. Unexpected successful runs become gating XPASS.
6. **Reliable continuity diagnostics.** `harness.rptparse.continuity_rows` and
   `python -m tools.compare_continuity LEGACY.rpt V6.rpt` match pollutants by
   name across legacy's multi-column and v6's repeated single-column tables.
   Non-finite values remain visible. The initial runoff-storage label is
   normalized. Report quality error extraction now includes every v6 pollutant.
7. **Quality system outflows.** Mirror `removeOutflows`, legacy
   `routing.c:1016-1042`: use the water ledger's system-outflow classification
   for pollutant exports at ordinary outfalls, non-DW terminal junctions and
   unponded flooding. Book negative lateral withdrawals at the mixed node
   concentration; do not count supplying outfalls as exports. Mirror
   `inlet_adjustQualOutflows` (`inlet.c:737-743`) by crediting pollutant mass
   returned from inlet capture nodes. Two integration regressions verify
   terminal-junction mass closure and the concentration carried by flooding.
   `ejemplo-epa` exports the same printed 0.036 lb as legacy, and its v6
   quality continuity error falls from 97% to 1.164%. Legacy's printed zero
   accompanies NaN ledger terms and is not a valid conservation reference.

## Validation

The original isolated source/build was `/private/tmp/swmm-parity-20260925/{src,build}`.
After that temporary directory disappeared, it was recovered under
`tests/output/parity_handoff_2026-09-25/recovery/{src,build}`. All 13 source/test
hashes match the original candidate, the AppleClang 21.0.0.21000334 compiler
and build flags match, and the pinned legacy executable hash is unchanged.
The recovered build again passes the 216/220 gate, and both previously
byte-identical target outputs remain byte-identical. Recovery logs and binary
hashes are retained in that persistent directory.
It starts from the engine base commit plus these changes and the existing,
uncommitted `InpWriter.hpp` build unblock described in the handoff. That
header is not part of this change. Compiler flags match the original gate,
including `-ffp-contract=off`; default Release flags are not parity-equivalent.

- Final engine ctest: **216/220**, exactly the standing `ard_node_store`,
  `msx_parity`, `dw_unsteady_friction`, and `xsect_parity` failures.
- Benchmark tests: **223 passed, 1 pre-existing failure**. The encoding guard
  flags eight calls in unchanged `tests/test_runtime_gate.py`; it also fails
  on a clean benchmark base snapshot. No unrelated test edits were made.
- All 17 shape-rejection exceptions were rerun against the pinned oracle;
  every case matched its specific expected diagnostic.
- Target output audit:

| Deck | Before over tolerance | After | Stronger result |
|---|---:|---:|---|
| swmm5buildupwashoff | 309 | 0 | result bytes identical |
| runoff42-sw5 | 7,000 | 0 | result bytes identical |
| ejemplo-epa | 15,442 | 0 | all finite element cells exact; 3,862 finite/NaN mismatches remain |
| treattest1 | 985 | 0 | all finite element cells exact; 42 finite/NaN mismatches remain |
| 193-h-h-si-units-elements | 13,658 | 10,835 | improved; remains FAIL |
| greenroofs | 5,846 | 4,707 | improved; remains FAIL |
| lid-imperviouscapture | 2,962 | 1,598 | improved; remains FAIL |
| 1943-h-h-elements-si-units | 89,714 | 7 | improved, not closed |
| v14rtcmanyrules | ERROR-MISMATCH | BOTH-ERROR | invalid action subtype is now rejected |

- `many-gages` report: substantive flow-continuity rows agree after separating
  seepage; the derived continuity-error row still differs (0.034 vs 0.040).
- Completed corpus validation: **1,071 unique cases**, with **zero baseline
  PASS regressions under the existing comparator**. Raw counts are 863 PASS,
  158 BOTH-ERROR, 31 FAIL, 17 ERROR-MISMATCH and 2 ERROR, versus 859 PASS,
  157 BOTH-ERROR, 35 FAIL, 18 ERROR-MISMATCH and 2 ERROR before this change.
  These are comparator labels, not a claim of universal byte equality or
  confirmed rejection agreement; the limitations below still apply.
- The six-worker sweep completed 1,070 cases before interruption. Its three
  known timeout cases (`3974-subs-horton`, `8367-nodes`, `9651-nodes`) and the
  unfinished `2009-nodes-si` were rerun sequentially on the recovered build;
  all four PASS with zero cells over tolerance. The 9651-node rerun reused
  the retained pinned-oracle output after matching input and executable hashes;
  its binary footer has error code zero and 144 periods.
- Merged results and the before/after audit are in benchmark
  `results/short_sweep/codex_verify38_final_scores.jsonl` and
  `results/short_sweep/codex_verify38_final_audit.json`. Original six-worker
  results and isolated rerun results remain separate for traceability.


Engine gate logs, binary hashes and the implementation patch are in
`tests/output/parity_handoff_2026-09-25/`. Benchmark diagnostics are in
`results/short_sweep/codex_target_exactness.json`,
`codex_benchmark_tests_final.log`, `codex_benchmark_tests_recovery.log`, and
`codex_benchmark_baseline_test.log`.

## Corrections to the supplied handoff

- `over=0` is not proof of literal byte equality. The current binary comparator
  ignores NaN mismatches. See legacy issue 5, appended to
  `LEGACY_TREATMENT_AND_VALIDATION_ISSUE_REPORT_2026-09-23.md`. Its strict parity
  requirement remains open; legacy's non-finite behavior was not injected into
  v6 as part of the finite-trajectory fix.
- The old continuity triage tool keys quality rows by column number, overwriting
  earlier v6 pollutants with the last repeated table. Its Family A conclusions
  must be reassessed using the new named-column comparison. It also discarded
  NaN ledger values, so apparently missing rows were often non-finite rows.
- All seven Family B LID decks already account for initial storage and match
  legacy at printed precision. `initMassBalance` adds `lid_.storedVolume()`;
  the report simply labels it `Initial Storage`. Remaining LID discrepancies
  need their own diagnosis; adding the term again would double count water.
- `v14rtcmanyrules` is not just WARNING 11: legacy later reports ERROR 209
  because line 494 says `THEN WEIR 8040 ...` while 8040 is a conduit.
- The standalone sweep driver treats its first report warning as a rejection
  when the other engine lacks readable output. **48** saved baseline BOTH-ERROR
  rows have one engine exiting zero with a readable binary. These need
  revalidation: `_usable_output` does not check the binary error code or a
  positive period count, and the saved rows retain only the first diagnostic.
  A readability-based reclassification gives 859 PASS, 109 BOTH-ERROR,
  35 FAIL, 66 ERROR-MISMATCH and 2 ERROR, but those are provisional, not
  confirmed corrected verdicts. Evidence is in benchmark
  `results/short_sweep/codex_baseline_rejection_audit.json`. Raw labels are
  retained for before/after comparison; they do not establish rejection parity.
  The new expected-rejection matcher explicitly requires an ERROR diagnostic.


## Remaining work

This implements the verified fixes above, not the entire parity programme.
The remaining LID, numerical/step-grid, hotstart and non-finite discrepancies
remain open. In particular, `wq-extran1` uses `testwq.hsf`:
`HotStartManager::apply_legacy_routing` currently reads and discards saved
pollutant concentrations and storage HRT; subsequent `initQuality` seeds
cold concentrations. Its legacy initial quality mass is nonzero while v6's
is zero. A fix needs ordering and hotstart restart tests, not an outfall-ledger
patch. Do not infer that these remaining issues were fixed by this round.

## Reproducing validation

Use the isolated build above while the shared checkout contains unrelated work.
The dependency toolchain is the existing vcpkg installation and the build uses
Ninja plus the `openswmm313` environment's CMake. Exact options are recorded in
`tests/output/parity_handoff_2026-09-25/build_options.txt`; source and executable
hashes are recorded beside it. Tests that use `tmpnam` need normal access to the
system temporary directory.

From the benchmarks repository, use the absolute
`results/short_sweep/codex_parity_bin` directory as `OPENSWMM_BIN_DIR` when
running `results/short_sweep/diag_case.py`. For `ejemplo-epa`, provide `--elem 2`;
for `treattest1`, provide `--elem 1` because the existing diagnostic does not
handle an unset worst element when only non-finite differences remain.

The six-worker sweep is `results/short_sweep/parallel_codex_verify38.py`.
For recovery runs, set `OPENSWMM_EXE` to the absolute recovered
`recovery/build/bin/Release/openswmm` path; the original default refers to
removed temporary storage. The sequential recovery driver is
`results/short_sweep/parallel_codex_verify38_alone.py`.
It resumes by skipping case IDs already in its scores file. Its raw verdicts
are directly comparable with `verify36_scores.jsonl`; it does not apply the
17 metadata exceptions. The regular `suites.parity.suite` path applies those
exceptions, whose diagnostics were independently checked against the oracle.

### Legacy defect evidence retained with this change

In legacy `qualrout.c:384-407`, the API pollutant-load addition follows the
negligible-volume branch. A zero API load at a dry conduit with zero inflow
adds `0 / (v1 + qIn * tStep)`, or `0 / 0`, to the concentration. The unchanged
`ejemplo-epa` deck demonstrates this at link 2 before the first wet period.
Legacy's External Inflow, Exfiltration Loss and Final Stored Mass quality rows
contain NaN even though it prints a zero continuity error. Literal legacy
parity would need to preserve these non-finite output cells and their binary
representation. That behavior remains open; finite equality is reported
separately above. The existing legacy issue report was also appended locally;
its earlier contents belong to the supplied handoff and were not staged.
