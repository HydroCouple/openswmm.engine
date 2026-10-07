# Residual-cache qualification checkpoint

This checkpoint preserves the performance-program reports, isolated candidate patch,
experiment drivers, source/binary manifests and compact results. The cache is **not
integrated into the production solver**. See the qualification report under
`plans/1d/DW_FV_RESIDUAL_QUALIFICATION_REPORT_2026-10-04.md`.

Large source/build snapshots, platform-specific libraries, per-run binary output
and full diagnostic logs remain local review artifacts; they are not committed.
Report links to those artifacts require the original workspace. Historical reports
also reference local results from earlier experiments. JSON manifests identify the
exact frozen sources and binaries used; they do not imply that the then-dirty
checkout can be reconstructed from HEAD alone.

The retained scripts use sibling `dw_fv_residual_2026-10-04` frozen source/library
snapshots and the reusable `dw_fv_p0_2026-10-03` build. Do not run preparation scripts
against a production checkout. Reproduction elsewhere requires staging the
manifest-matched snapshots and compatible build first. The qualification scripts
inject one test-only rejection; attribution uses separate libraries without that
injection. Neither diagnostic build is a performance acceptance benchmark.

The snapshot's `commit_created: false` records the status when measurements were
finished, before this later user-requested archival commit. No production change
or numerical-default change is included in this checkpoint.
