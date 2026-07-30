# MSVC String Literal Limit — GeoPackage Schema DDL

**Status:** proposed, awaiting review
**Date:** 2026-07-29

## 1. Confirmation of the problem

MSVC error **C2026** — *"string too big, trailing characters truncated"*. Per Microsoft's
documentation the limit is **16,380 single-byte characters per string literal**, and the limit
applies **before** adjacent literals are concatenated. (Concatenated result has a separate,
much higher limit of 65,535 bytes — error C1091.)

Repo-wide scan of `src/`, `include/`, `tests/`, `tools/`, `packages/` for raw string literals
over 16,000 bytes returned exactly **one** offender:

| Symbol | File | Line | Bytes |
|---|---|---|---|
| `PART_A_DDL` | `src/engine/input/geopackage/GeoPackageSchema.cpp` | 66 | **20,727** |

Next largest literals are well under the limit and need no action:

| Symbol | Bytes |
|---|---|
| `PART_D_DDL` | 10,866 |
| `MESH_2D_DDL` | 6,321 |
| `PART_B_DDL`, `PART_C_DDL`, `GPKG_METADATA_DDL` | < 6,000 |

Windows/MSVC **is** in CI (`.github/workflows/unit_testing.yml`, `windows-latest` /
`x64-windows`), so this is a hard build failure on that leg, not a latent risk.

### Scope facts that make this safe

- All six `*_DDL` symbols are `static` (internal linkage) in one `.cpp`. Only consumer is
  `create_schema()` in the same file. No header, test, or other TU references them
  (verified by grep).
- `exec()` in `GpkgUtils.hpp` takes `const std::string&`, so `const char[]` binds fine.
- `create_schema()` already issues six independent `exec()` calls; adding more is consistent
  with the existing design.
- Splitting at statement boundaries does not change the resulting schema. SQLite resolves
  foreign keys lazily (a `CREATE TABLE` referencing a not-yet-created parent succeeds;
  the check happens at DML time), and all `CREATE INDEX` statements sit adjacent to their
  own table, so no index crosses a proposed boundary.

## 2. Options considered

**Option 1 — adjacent raw-literal concatenation (smallest diff).**
Keep one `PART_A_DDL` symbol, split its body into two adjacent `R"SQL(...)SQL"` literals.
`create_schema()` untouched.
*Rejected:* the fix is invisible at the point of maintenance — nothing stops a future edit
from growing one half past 16,380 again, and no compile-time guard is possible once the
literals are concatenated.

**Option 2 — split into named chunks with a compile-time guard (recommended).**
Mirrors the existing `PART_A/B/C/D` decomposition, makes each chunk independently
size-checkable, and fails the build on *every* compiler if a chunk grows too large — not
just on the Windows CI leg.

## 3. Recommended implementation

### 3.1 Split `PART_A_DDL` into four domain-aligned chunks

Boundaries chosen at existing comment-block headers, preserving statement order exactly.
Current line numbers in `GeoPackageSchema.cpp`:

| New symbol | Source lines | Bytes | Tables covered |
|---|---|---|---|
| `PART_A1_DDL` | 67–157 | 3,496 | `options`, `nodes`, `storages`, `outfalls`, `dividers` |
| `PART_A2_DDL` | 158–270 | 4,394 | `links`, `conduits`, `pumps`, `orifices`, `weirs`, `outlets` |
| `PART_A3_DDL` | 271–423 | 5,513 | `subcatchments`, `rain_gages`, `node_links`, `subcatch_routing`, `curves`, `input_timeseries`, `patterns`, `inflows`, `dwf_inflows` (+ their indexes) |
| `PART_A4_DDL` | 424–636 | 7,531 | `control_rules`, `evaporation`, `climate_settings`, `snowpacks`, `adjustments`, `subcatch_adjustments`, `pollutants`, `lid_controls`, `lid_usage`, `rdii_assignments`, `unit_hydrographs`, `rdii_decay`, `treatment`, `transects` (+ their indexes) |

Largest chunk is 7,531 bytes — 2.2× headroom under the 16,380 limit.

### 3.2 Change declarations from `const char*` to `const char[]`

Required so `sizeof` yields the literal length. Applied to **all six** DDL symbols so the
guard is uniform:

```cpp
static const char PART_A1_DDL[] = R"SQL(
...
)SQL";
static_assert(sizeof(PART_A1_DDL) <= 16380, "MSVC C2026: split this DDL chunk");
```

Six identical `static_assert` lines (one per symbol). No macro — a macro for six call sites
is the kind of abstraction CLAUDE.md §2 warns against.

### 3.3 Update `create_schema()`

```cpp
exec(db, GPKG_METADATA_DDL);
exec(db, PART_A1_DDL);
exec(db, PART_A2_DDL);
exec(db, PART_A3_DDL);
exec(db, PART_A4_DDL);
exec(db, PART_B_DDL);
exec(db, PART_C_DDL);
exec(db, PART_D_DDL);
exec(db, MESH_2D_DDL);
```

### 3.4 Doc comment

`GeoPackageSchema.hpp`'s `create_schema()` docblock lists "Part A: ..." tables. Update only
if the wording is made stale by the split — the table list itself is unchanged, so likely a
no-op. No other header change.

**Nothing else changes.** No new files, no schema change, no behavior change.

## 4. Verification plan

Files written to `test_artifacts/msvc_literal_check/` (per CLAUDE.md §4.1), not temp dirs.

1. **Byte-identity of the SQL** → the strongest check. Extract `PART_A_DDL` from
   `git show HEAD:src/engine/input/geopackage/GeoPackageSchema.cpp`, extract and concatenate
   `PART_A1..A4_DDL` from the working tree, assert the two byte strings are identical.
   *Verify:* diff is empty.
2. **Compile-time guard fires** → temporarily lower one `static_assert` bound below a chunk's
   size, confirm the build fails; restore.
   *Verify:* expected compiler error, then clean build.
3. **Local build + targeted tests** → configure and build, then run the GeoPackage suite:
   `test_geopackage`, `test_geopackage_mesh2d`, `test_geopackage_formats`,
   `test_geopackage_schema_external_content`,
   `test_geopackage_external_content_{reader,writer}`.
   *Verify:* all pass.
4. **Schema equivalence at runtime** → dump `sqlite_master` (name, sql) from a database
   created by the patched `create_schema()`; diff against a dump from a pre-change build,
   saved as `test_artifacts/msvc_literal_check/schema_{before,after}.txt`.
   *Verify:* diff is empty.
5. **Repo-wide rescan** → re-run the >16,000-byte literal scan.
   *Verify:* zero hits.
6. **Windows CI** → push branch, confirm the `windows-latest` / `x64-windows` leg builds
   and its tests pass.
   *Verify:* green.

Steps 1–5 are local; step 6 is the actual acceptance criterion.

## 5. Open questions for review

1. **Four chunks vs. two?** Two chunks (~10.3 KB each) is a smaller diff but leaves only
   1.6× headroom. Four gives 2.2×. Preference?
2. **Guard threshold.** Proposed `<= 16380` (the exact MSVC limit). A tighter bound such as
   `<= 12000` would flag growth before it becomes urgent. Preference?
3. **`static_assert` on the other five symbols** — included above for uniformity, but it
   touches lines that aren't broken. Drop them and guard only `PART_A*` if you'd rather keep
   the diff strictly minimal.
4. **CHANGELOG.md** — per CLAUDE.md §5.2 this is release-time; confirm whether this build fix
   warrants an entry now.
