#!/usr/bin/env bash
# ============================================================================
# scripts/clean.sh
#
# Wipe every build artefact that scikit-build-core, CMake, Cython, pytest,
# and pip might leave inside the python/ tree.  Use this whenever:
#
#   - You switch Python versions (cp312 ↔ cp313) and want to drop stale
#     .so/.pyd files from the previous ABI.
#   - You toggle DEBUG / Release builds.
#   - Editable installs misbehave because old artefacts are shadowing new
#     CMake outputs.
#   - You're about to publish an sdist or wheel and want to be sure no
#     local binaries leak into it.
#
# Run from the python/ directory:
#
#     ./scripts/clean.sh
#
# Add `--dry-run` to print what would be removed without deleting anything.
# ============================================================================

set -euo pipefail

dry_run=0
case "${1:-}" in
    -n|--dry-run)
        dry_run=1
        ;;
esac

cd "$(dirname "$0")/.."

remove() {
    if [[ $dry_run -eq 1 ]]; then
        printf '[dry-run] would remove %s\n' "$@"
    else
        rm -rf "$@"
    fi
}

# ── Build directories ────────────────────────────────────────────────────────
remove build _skbuild dist wheelhouse openswmm.egg-info

# ── Compiled extensions and bundled shared libraries inside the package ─────
find openswmm \
        \( -name '*.so'    \
        -o -name '*.pyd'   \
        -o -name '*.dylib' \
        -o -name '*.dll'   \
        -o -name '*.lib'   \
    \) -type f -print | while read -r f; do
        remove "$f"
done
find openswmm \
        \( -name 'lib*.dylib' \
        -o -name 'lib*.so' \
    \) -type l -print | while read -r f; do
        remove "$f"
done

# ── Python bytecode / pytest cache / IDE clutter ─────────────────────────────
find . -type d \( \
        -name '__pycache__' \
        -o -name '.pytest_cache' \
    \) -prune -print | while read -r d; do
        remove "$d"
done

# Caches, .DS_Store
find . -type f \( -name '*.pyc' -o -name '*.pyo' -o -name '.DS_Store' \) \
    -print | while read -r f; do
        remove "$f"
done

if [[ $dry_run -eq 0 ]]; then
    echo "clean.sh: done"
fi
