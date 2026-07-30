# Verification Instructions — MSVC String Literal Split (GeoPackage Schema DDL)

**For:** an independent verifying agent, working from a clean checkout.
**Subject commit:** the commit whose message begins `fix(geopackage): split PART_A_DDL`
**Plan of record:** `plans/MSVC_STRING_LITERAL_LIMIT_PLAN.md`
**Date written:** 2026-07-29

You are verifying a **build-portability fix only**. If you find yourself changing SQL,
adding tables, or touching anything outside `GeoPackageSchema.cpp`, stop — that is out of
scope and means something went wrong.

---

## 0. What was changed and why

MSVC error **C2026** limits a single string literal to **16,380 single-byte characters**,
applied *before* adjacent literals are concatenated
(<https://learn.microsoft.com/en-us/cpp/error-messages/compiler-errors-1/compiler-error-c2026>).
`PART_A_DDL` in `src/engine/input/geopackage/GeoPackageSchema.cpp` was **20,727 bytes** and
therefore could not compile under MSVC. Windows/MSVC is in CI
(`.github/workflows/unit_testing.yml`, `windows-latest` / `x64-windows`).

The fix:

1. `PART_A_DDL` split into `PART_A1_DDL` … `PART_A4_DDL` at existing comment-block /
   statement boundaries. **No SQL text was added, removed, or reordered.**
2. All nine DDL symbols changed from `static const char*` to `static const char[]` so that
   `sizeof` yields the literal length.
3. A `static_assert(sizeof(X) <= 16380, "MSVC C2026: split this DDL chunk");` added after
   each of the nine symbols, so regrowth fails the build on *every* compiler rather than
   only on the Windows CI leg.
4. `create_schema()`'s single `exec(db, PART_A_DDL)` replaced by four `exec()` calls in the
   same order.

Expected diff size: **36 insertions, 7 deletions, one file.**

### Known-and-intended byte difference

Reopening a raw string literal injects one leading `\n` into each of chunks A2, A3, A4
(the newline immediately after `R"SQL(` is part of the literal). The naive concatenation
of A1..A4 is therefore **20,730 bytes vs. the original 20,727**. Those 3 bytes are
inter-statement whitespace and are semantically inert. Step 2 below normalizes for them.
**If you see a 3-byte delta, that is correct.** Any other delta is a real defect.

---

## 1. Confirm the scope of the diff

```bash
git show --stat <commit> -- src/engine/input/geopackage/GeoPackageSchema.cpp
git show -U0 <commit> -- src/engine/input/geopackage/GeoPackageSchema.cpp
```

**Pass criteria:**

- Exactly one source file touched (plus `plans/*.md` docs).
- Every added line is one of: a `)SQL";` terminator, a `static_assert(...)`, a
  `static const char <NAME>[] = R"SQL(` declaration, a `//` comment, a blank line, or an
  `exec(db, PART_A<n>_DDL);` call.
- **Zero lines beginning with `CREATE`, `INSERT`, `--`, or a column definition appear as
  added or removed.** If any SQL line shows up in the diff, the split corrupted the DDL.

## 2. Prove the SQL is byte-identical to the pre-change version

This is the strongest check. Run from the repo root:

```bash
mkdir -p test_artifacts/msvc_literal_check
python3 - <<'PY'
import re, subprocess, pathlib
P='src/engine/input/geopackage/GeoPackageSchema.cpp'
OUT=pathlib.Path('test_artifacts/msvc_literal_check')
def lit(src,name):
    m=re.search(r'static const char\*?\s*'+name+r'(\[\])?\s*=\s*R"SQL\(',src)
    st=m.end(); return src[st:src.find(')SQL"',st)]
head=subprocess.run(['git','show','<commit>^:'+P],capture_output=True,text=True,check=True).stdout
work=open(P,encoding='utf-8').read()
before=lit(head,'PART_A_DDL')
chunks=[lit(work,n) for n in ('PART_A1_DDL','PART_A2_DDL','PART_A3_DDL','PART_A4_DDL')]
after=chunks[0]+''.join(c[1:] if c.startswith('\n') else c for c in chunks[1:])
(OUT/'part_a_before.sql').write_text(before); (OUT/'part_a_after.sql').write_text(after)
print('before bytes:', len(before), ' after bytes:', len(after))
print('BYTE-IDENTICAL:', before==after)
print('raw concat (expect before+3):', sum(len(c) for c in chunks))
PY
```

**Pass criteria:** `BYTE-IDENTICAL: True`, `before bytes: 20727`, raw concat `20730`.

Artifacts land in `test_artifacts/msvc_literal_check/` for manual inspection — do not use
temp directories (CLAUDE.md §4.1).

## 3. Prove the resulting database schema is unchanged

Builds the schema twice via Python's `sqlite3` — once from the pre-change symbol sequence,
once from the post-change sequence — and diffs `sqlite_master`.

```bash
python3 - <<'PY'
import re, subprocess, sqlite3, pathlib
P='src/engine/input/geopackage/GeoPackageSchema.cpp'
OUT=pathlib.Path('test_artifacts/msvc_literal_check'); OUT.mkdir(parents=True, exist_ok=True)
def lit(src,name):
    m=re.search(r'static const char\*?\s*'+name+r'(\[\])?\s*=\s*R"SQL\(',src)
    st=m.end(); return src[st:src.find(')SQL"',st)]
head=subprocess.run(['git','show','<commit>^:'+P],capture_output=True,text=True,check=True).stdout
work=open(P,encoding='utf-8').read()
OLD=['GPKG_METADATA_DDL','PART_A_DDL','PART_B_DDL','PART_C_DDL','PART_D_DDL','MESH_2D_DDL']
NEW=['GPKG_METADATA_DDL','PART_A1_DDL','PART_A2_DDL','PART_A3_DDL','PART_A4_DDL',
     'PART_B_DDL','PART_C_DDL','PART_D_DDL','MESH_2D_DDL']
def build(src,names):
    db=sqlite3.connect(':memory:')
    db.executescript("PRAGMA foreign_keys=ON; PRAGMA application_id=0x47504B47;")
    for n in names: db.executescript(lit(src,n))
    rows=db.execute("SELECT type,name,sql FROM sqlite_master ORDER BY type,name").fetchall()
    nt=db.execute("SELECT count(*) FROM sqlite_master WHERE type='table'").fetchone()[0]
    ni=db.execute("SELECT count(*) FROM sqlite_master WHERE type='index' AND sql IS NOT NULL").fetchone()[0]
    fk=db.execute("PRAGMA foreign_key_check").fetchall()
    return '\n'.join(f'{t}\t{n}\n{s}\n' for t,n,s in rows), nt, ni, fk
b,bt,bi,bfk=build(head,OLD); a,at,ai,afk=build(work,NEW)
(OUT/'schema_before.txt').write_text(b); (OUT/'schema_after.txt').write_text(a)
print(f'tables  before={bt} after={at}')
print(f'indexes before={bi} after={ai}')
print(f'fk violations before={len(bfk)} after={len(afk)}')
print('sqlite_master IDENTICAL:', a==b)
PY
```

**Pass criteria:** `tables 63/63`, `indexes 19/19`, `fk violations 0/0`,
`sqlite_master IDENTICAL: True`.

Note: do **not** enable `PRAGMA journal_mode=WAL` in this check if the repo lives on a
network or container-mounted filesystem — it raises `disk I/O error` there and is
irrelevant to schema equivalence.

## 4. Confirm the `static_assert` guard actually fires

A guard that never fails is not a guard. Deliberately break it and confirm the build stops:

```bash
SQ=$(find . -name sqlite3.h -path '*vcpkg_installed*' -print -quit | xargs dirname)
F=src/engine/input/geopackage/GeoPackageSchema.cpp
cp "$F" /tmp/orig.cpp
sed -i 's/sizeof(PART_A4_DDL) <= 16380/sizeof(PART_A4_DDL) <= 7000/' "$F"
g++ -fsyntax-only -std=c++20 -I include -I src -I "$SQ" "$F"    # EXPECT: failure
cp /tmp/orig.cpp "$F"
g++ -fsyntax-only -std=c++20 -I include -I src -I "$SQ" "$F" && echo CLEAN
git diff --quiet "$F" && echo "RESTORED"
```

**Pass criteria:** first invocation emits
`error: static assertion failed: MSVC C2026: split this DDL chunk`; second prints `CLEAN`;
`RESTORED` confirms you left no residue.

## 5. Repo-wide rescan for new offenders

```bash
python3 - <<'PY'
import re, os
hits=0
for root in ('src','include','tests','tools','packages'):
    for dp,dn,fn in os.walk(root):
        dn[:]=[d for d in dn if d not in ('build','.venv','vcpkg_installed','node_modules')]
        for f in fn:
            if not f.endswith(('.cpp','.hpp','.h','.cc','.cxx')): continue
            p=os.path.join(dp,f); s=open(p,encoding='utf-8',errors='replace').read()
            for m in re.finditer(r'R"([A-Za-z_]*)\(',s):
                tag=m.group(1); n=s.find(')'+tag+'"',m.end())-m.end()
                if n>16000: print(p,n); hits+=1
print('hits:',hits)
PY
```

**Pass criteria:** `hits: 0`.

## 6. Full build + GeoPackage test suite  ← NOT YET DONE

**This step was not performed by the implementing agent** and is the main thing you are
being asked to add. The implementer had only a Linux sandbox without the vcpkg dependency
tree, so verification stopped at `g++ -fsyntax-only` on the single translation unit
(which does evaluate the `static_assert`s, but does not link or run anything).

Configure and build with the project's normal preset, then run:

```bash
ctest -R 'geopackage' --output-on-failure
```

Targets that must pass:

- `test_geopackage`
- `test_geopackage_mesh2d`
- `test_geopackage_formats`
- `test_geopackage_schema_external_content`
- `test_geopackage_external_content_reader`
- `test_geopackage_external_content_writer`

Also run the writer/reader round-trip regression suite if the project has one that produces
a `.gpkg` — the split affects table creation order within Part A, and while SQLite resolves
foreign keys lazily (so ordering cannot matter at DDL time), an end-to-end write→read
round-trip is the check that proves it empirically rather than by argument.

**Pass criteria:** all green, and no new warnings attributable to `GeoPackageSchema.cpp`.

## 7. Windows / MSVC CI — the actual acceptance criterion  ← NOT YET DONE

Everything above is circumstantial. The fix is only proven when the compiler that rejected
the code accepts it.

Push the branch and confirm the `windows-latest` / `x64-windows` leg of
`.github/workflows/unit_testing.yml` **builds and passes**. Before this change that leg
should have been failing with C2026 on `GeoPackageSchema.cpp`; confirm from the previous
run's log that it was, so you know you fixed the actual failure and not a different one.

**Pass criteria:** Windows leg green; C2026 absent from the build log.

---

## Report back

State pass/fail per step 1–7. For any failure, give the exact command, the observed output,
and your assessment of whether it indicates a defect in this change or a pre-existing
condition. Do not fix anything you find in step 6 or 7 without flagging it first — if the
Windows build reveals *additional* C2026 sites or unrelated MSVC errors, those are separate
work items, not part of this change.

## Things that are deliberately NOT in scope

- `PART_D_DDL` is 10,866 bytes — under the limit, left alone.
- No `CHANGELOG.md` entry was added; per CLAUDE.md §5.2 that happens at release time.
- `GeoPackageSchema.hpp`'s `create_schema()` docblock was not edited: it lists the Part A
  *tables*, which are unchanged.
- The alternative fix (adjacent raw-literal concatenation into a single symbol) was
  considered and rejected — see `plans/MSVC_STRING_LITERAL_LIMIT_PLAN.md` §2. Do not
  "simplify" the change back to that form; it admits no compile-time guard.
