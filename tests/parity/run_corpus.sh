#!/usr/bin/env bash
#
# run_corpus.sh -- the bit-identity corpus, before and after.
#
#   run_corpus.sh <base-cli> <patched-cli> <outdir>
#
# Runs every deck in tests/parity/MANIFEST through BOTH binaries and compares
# the .out files byte for byte.  Exit 0 only if every deck is identical and
# every run produced an .out.
#
# There are no stored baselines here on purpose.  A stored baseline has to be
# regenerated on every intentional change, and regenerating it is exactly the
# moment a wrong number gets blessed.  Two binaries and a cmp needs no
# blessing.  (The snow deck additionally keeps a stored baseline with recorded
# provenance under snow/baseline/ -- that is a separate mechanism for a deck
# that is EXPECTED to move and needs its movement attributed.)
#
# The .out is the artefact compared, never the .rpt: the report carries a run
# timestamp and would differ on every invocation.
#
set -u

usage() {
    cat >&2 <<'EOF'
usage: run_corpus.sh <base-cli> <patched-cli> <outdir>

  base-cli     openswmm built at the base commit
  patched-cli  openswmm built with the changeset applied
  outdir       written fresh; base/ and patched/ are created under it

Both binaries must be built the SAME WAY -- same preset, same build type.
A corpus run comparing a Release base against a Debug patched measures the
build, not the changeset.
EOF
    exit 2
}

[ $# -eq 3 ] || usage

BASE_CLI=$1
PATCHED_CLI=$2
OUTDIR=$3

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$HERE/../.." && pwd)
MANIFEST=$HERE/MANIFEST

for f in "$BASE_CLI" "$PATCHED_CLI"; do
    [ -x "$f" ] || { echo "not executable: $f" >&2; exit 2; }
done
[ -f "$MANIFEST" ] || { echo "no manifest: $MANIFEST" >&2; exit 2; }

# ---- read the manifest ----------------------------------------------------
DECKS=()
while IFS=$'\t' read -r path _reason; do
    case "$path" in ''|'#'*) continue ;; esac
    path=${path%%[[:space:]]}
    [ -f "$ROOT/$path" ] || { echo "manifest names a missing deck: $path" >&2; exit 2; }
    DECKS+=("$path")
done < "$MANIFEST"

[ ${#DECKS[@]} -gt 0 ] || { echo "manifest is empty" >&2; exit 2; }

# Every deck writes its .out under its own basename, so two decks sharing a
# basename would silently overwrite each other and compare clean.  This is
# the corpus-level form of the fixture-name collision fixed in b85b802d.
DUPES=$(for p in "${DECKS[@]}"; do basename "$p" .inp; done | sort | uniq -d)
if [ -n "$DUPES" ]; then
    echo "manifest has decks sharing a basename -- rename one:" >&2
    echo "$DUPES" >&2
    exit 2
fi

# ---- provenance -----------------------------------------------------------
rm -rf -- "$OUTDIR"
mkdir -p -- "$OUTDIR/base" "$OUTDIR/patched"
PROV=$OUTDIR/PROVENANCE.txt
sha() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1 || echo "?"; }

# THE CLI IS NOT THE ENGINE.  `openswmm` is a thin driver that links
# @rpath/libopenswmm.engine.<v>.dylib, so two builds of DIFFERENT SOURCE
# produce BYTE-IDENTICAL CLI executables -- measured: an engine changeset of
# 143 changed lines left bdd99f49... on both sides.  Hashing only the CLI
# therefore says nothing about which engine ran, and a run that copies one
# CLI aside and rebuilds in the SAME build directory compares one engine
# against itself while looking like a real before/after.
#
# So resolve each CLI's engine library and hash that too.  @rpath cannot be
# resolved portably from shell; the two places it ever is are beside the exe
# and ../lib, which covers the build tree and the install layout.
engine_lib() {
    local dir; dir=$(cd -- "$(dirname -- "$1")" && pwd)
    local c
    # beside the exe (staged build tree, install bin/), ../lib (prefix
    # layout), ../engine (the CMake build tree, where src/cli/openswmm sits
    # next to src/engine/libopenswmm.engine.*).
    for c in "$dir"/libopenswmm.engine*.dylib "$dir"/libopenswmm.engine*.so \
             "$dir"/../lib/libopenswmm.engine*.dylib \
             "$dir"/../lib/libopenswmm.engine*.so \
             "$dir"/../engine/libopenswmm.engine*.dylib \
             "$dir"/../engine/libopenswmm.engine*.so \
             "$dir"/openswmm.engine*.dll "$dir"/../engine/openswmm.engine*.dll; do
        [ -f "$c" ] && { echo "$c"; return; }
    done
    echo ""
}
BASE_LIB=$(engine_lib "$BASE_CLI")
PATCHED_LIB=$(engine_lib "$PATCHED_CLI")

{
    echo "corpus run: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "decks:      ${#DECKS[@]}"
    echo "commit:     $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')"
    echo "base    $BASE_CLI"
    echo "        sha256 $(sha "$BASE_CLI")"
    echo "        engine ${BASE_LIB:-<unresolved>}"
    echo "        sha256 $([ -n "$BASE_LIB" ] && sha "$BASE_LIB" || echo '?')"
    echo "patched $PATCHED_CLI"
    echo "        sha256 $(sha "$PATCHED_CLI")"
    echo "        engine ${PATCHED_LIB:-<unresolved>}"
    echo "        sha256 $([ -n "$PATCHED_LIB" ] && sha "$PATCHED_LIB" || echo '?')"
} > "$PROV"

# Vacuous if the ENGINE is the same on both sides -- the CLI alone is not the
# question.  Say so; do not fail, since running one build against itself is a
# legitimate way to check the harness.
if [ -z "$BASE_LIB" ] || [ -z "$PATCHED_LIB" ]; then
    echo "NOTE: could not resolve an engine library beside either binary;" \
         "the CLI hash alone cannot tell you whether the engines differ."
    echo "NOTE: engine library unresolved." >> "$PROV"
elif [ "$(sha "$BASE_LIB")" = "$(sha "$PATCHED_LIB")" ]; then
    echo "NOTE: base and patched resolve the SAME engine library" \
         "-- this run cannot fail."
    echo "NOTE: identical engine library on both sides." >> "$PROV"
    if [ "$BASE_LIB" = "$PATCHED_LIB" ]; then
        echo "NOTE: literally the same file. Two builds need two build" \
             "DIRECTORIES; copying one CLI aside does not copy its engine."
    fi
fi

# ---- the OTHER direction, which is the dangerous one -----------------------
#
# The check above asks "are these secretly the SAME build?" -- a run that
# cannot fail. The failure mode that actually cost a round is the mirror of
# it: two builds that differ by more than the changeset. On 2026-08-22 a
# corpus run reported FOUR moved decks -- force_legacy, force_ard,
# orif_legacy, sdm_struct_dw_ard, precisely the quality decks, exactly what a
# washoff-guard defect would look like. It was not the changeset. The two
# build directories had different CMake options
# (OPENSWMM_FAST_MANNING_POW / OPENSWMM_FAST_XSECT_LOOKUP OFF in one, ON in
# the other), so the run measured an xsect accelerator. A matched pair gave
# 18/18.
#
# A moved deck is the loudest signal this tool produces, and it was wrong in
# the most plausible possible way. So diff the two caches on the options that
# can change results, and say so BEFORE the table rather than after.
# Entries that pass the filter above but CANNOT change a number: which extra
# targets get built, the version strings compiled into the banner, and where
# things install.  They are excluded because the first real matched pair --
# build/darwin against build/darwin-tests-release, the two directories that
# produced the trustworthy 18/18 -- differed on OPENSWMM_BUILD_TESTS and on
# OPENSWMM_PRERELEASE, and raised the loud banner over a version string.  A
# warning that fires on every long-lived pair of build directories is a
# warning nobody reads, which is the failure this guard exists to avoid one
# level up.
#
# OPENSWMM_BUILD_2D and OPENSWMM_BUILD_GPU_PLUGIN match `BUILD_` and are
# deliberately NOT here: they change what the engine computes.  This is a
# named list rather than a `BUILD_*` pattern for exactly that reason.
CFG_IGNORE='^OPENSWMM_(BUILD_(TESTS|UNIT_TESTS|REGRESSION_TESTS|BENCHMARKS|PYTHON|O4_DIFFERENTIAL)|PRERELEASE|LEGACY_PRERELEASE|INSTALL|GPU_PLUGIN_INSTALL_DESTINATION):'

cache_opts() {
    local dir; dir=$(cd -- "$(dirname -- "$1")" && pwd)
    local c
    for c in "$dir/CMakeCache.txt" "$dir/../CMakeCache.txt" \
             "$dir/../../CMakeCache.txt" "$dir/../../../CMakeCache.txt"; do
        if [ -f "$c" ]; then
            grep -E '^(OPENSWMM_|CMAKE_BUILD_TYPE|CMAKE_CXX_FLAGS)[A-Z_0-9]*(:[A-Z]+)?=' \
                 "$c" 2>/dev/null | grep -Ev "$CFG_IGNORE" | sort
            return
        fi
    done
}
BASE_OPTS=$(cache_opts "$BASE_CLI")
PATCHED_OPTS=$(cache_opts "$PATCHED_CLI")

if [ -z "$BASE_OPTS" ] || [ -z "$PATCHED_OPTS" ]; then
    echo "NOTE: no CMakeCache.txt found beside one or both binaries, so the" \
         "build CONFIGURATIONS were not compared. A moved deck may be a" \
         "build-option difference rather than a code change."
    echo "NOTE: build configurations not compared (no CMakeCache)." >> "$PROV"
elif [ "$BASE_OPTS" != "$PATCHED_OPTS" ]; then
    echo "⛔ THE TWO BUILDS ARE CONFIGURED DIFFERENTLY. Any moved deck below"
    echo "   may be the configuration, not the changeset. Differences:"
    diff <(printf '%s\n' "$BASE_OPTS") <(printf '%s\n' "$PATCHED_OPTS") \
        | sed 's/^/     /'
    {
        echo "WARNING: build configurations DIFFER:"
        diff <(printf '%s\n' "$BASE_OPTS") <(printf '%s\n' "$PATCHED_OPTS")
    } >> "$PROV"
else
    echo "build configurations match on OPENSWMM_*, CMAKE_BUILD_TYPE and" \
         "CMAKE_CXX_FLAGS." >> "$PROV"
fi

# ---- run ------------------------------------------------------------------
# Each deck runs with its own output directory as the working directory, so a
# deck that writes a scratch file cannot reach another deck's.
run_side() {
    local cli=$1 side=$2 deck name dest
    for deck in "${DECKS[@]}"; do
        name=$(basename "$deck" .inp)
        dest=$OUTDIR/$side/$name
        mkdir -p "$dest"
        ( cd "$dest" && "$cli" "$ROOT/$deck" "$name.rpt" "$name.out" \
              > "$name.stdout" 2>&1 ) \
            || echo "$name" >> "$OUTDIR/$side/NONZERO_EXIT"
    done
}
run_side "$BASE_CLI"    base
run_side "$PATCHED_CLI" patched

# ---- compare --------------------------------------------------------------
printf '\n%-32s %s\n' "deck" "result"
printf '%-32s %s\n' "--------------------------------" "------"
moved=0; missing=0
for deck in "${DECKS[@]}"; do
    name=$(basename "$deck" .inp)
    a=$OUTDIR/base/$name/$name.out
    b=$OUTDIR/patched/$name/$name.out
    if [ ! -s "$a" ] || [ ! -s "$b" ]; then
        printf '%-32s %s\n' "$name" "NO .out  <-- run failed, see stdout"
        missing=$((missing+1))
    elif cmp -s "$a" "$b"; then
        printf '%-32s %s\n' "$name" "identical"
    else
        # cmp -l lists differing byte POSITIONS and stops at the shorter
        # file, so a pure length change counts zero.  Report the size delta
        # separately or a truncated .out reads as "DIFFERS (0 bytes)".
        sa=$(wc -c < "$a" | tr -d ' '); sb=$(wc -c < "$b" | tr -d ' ')
        if [ "$sa" != "$sb" ]; then
            detail="size $sa -> $sb"
        else
            detail="$(cmp -l "$a" "$b" 2>/dev/null | wc -l | tr -d ' ') of $sa bytes"
        fi
        printf '%-32s %s\n' "$name" "DIFFERS  ($detail)"
        moved=$((moved+1))
    fi
done

echo
for side in base patched; do
    if [ -f "$OUTDIR/$side/NONZERO_EXIT" ]; then
        echo "nonzero exit under $side: $(tr '\n' ' ' < "$OUTDIR/$side/NONZERO_EXIT")"
    fi
done

echo "$((${#DECKS[@]} - moved - missing))/${#DECKS[@]} identical, $moved moved, $missing missing"
echo "provenance: $PROV"

# A moved deck is not automatically a defect -- the snow deck is expected to
# move on a snow change.  It is always something to ATTRIBUTE, which is why
# this exits nonzero and makes you say so out loud.
[ $moved -eq 0 ] && [ $missing -eq 0 ]
