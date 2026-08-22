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
