#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# ---------------------------------------------------------------------------
# load_parity.sh — the parity gate for the model-load optimization program
# (plan Phase 0.4).
#
# Several of the planned optimizations change lookup or iteration order (hash
# instead of linear scan, memoized transect tables, bulk SoA growth). None of
# them may change results. For each model this gate compares three artifacts:
#
#   (a) the .rpt with wall-clock/version lines masked out
#   (b) the .out binary, byte for byte
#   (c) the written-back .inp (the parsed+resolved model)
#
# Usage:
#   load_parity.sh record <probe> <ref-dir> [model ...]
#   load_parity.sh check  <probe> <ref-dir> [model ...]
#   load_parity.sh selfcheck <probe> [model ...]
#
#   record     capture reference artifacts with the CURRENT build
#   check      re-run and diff against a previously recorded reference
#   selfcheck  record then check with the same binary — proves the engine's
#              own output is deterministic, so a later `check` failure means a
#              real behavioral change rather than run-to-run noise
#
# <probe> is the parity_probe executable (tests/benchmarks/parity_probe.cpp).
# With no explicit models, the default set below is used.
# ---------------------------------------------------------------------------
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

# Lines carrying a wall clock, a duration or a build stamp. Same masking policy
# as tests/output/xsect_adoption/byte_gate.py — everything else in the .rpt,
# including every continuity figure and summary table, is part of the gate.
VOLATILE_RE='(Analysis begun on|Analysis ended on|Total elapsed time|Version|Build|OpenSWMM [0-9])'

# Default models: the committed regression decks plus a spread of unit-test
# decks that between them exercise curves, timeseries, transects, streets,
# storage and quality — the surfaces Phases 1-3 touch.
default_models() {
    local candidates=(
        "tests/regression/data/Example1.inp"
        "tests/regression/data/cn_regen_parity.inp"
        "tests/unit/engine/data/warnerr_base.inp"
        # STREET cross-sections: the three decks above carry none, so without
        # this the gate silently skipped the per-link transect build that the
        # STREET/CUSTOM memoization changes.
        "tests/unit/engine/data/street_xsect.inp"
        # IRREGULAR cross-sections, for the transect name-resolution path.
        "tests/benchmarks/generated/transect_heavy.inp"
    )
    local m
    for m in "${candidates[@]}"; do
        [[ -f "${REPO_ROOT}/${m}" ]] && printf '%s\n' "${REPO_ROOT}/${m}"
    done
}

usage() {
    sed -n '18,42p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
}

# Scratch dirs are file-scope, not function-locals: the EXIT trap fires after
# the function has returned, and under `set -u` a local would be unbound there.
SCRATCH_A=""
SCRATCH_B=""
cleanup() {
    [[ -n "${SCRATCH_A}" ]] && rm -rf "${SCRATCH_A}"
    [[ -n "${SCRATCH_B}" ]] && rm -rf "${SCRATCH_B}"
    return 0
}
trap cleanup EXIT

# Emit a stable, comparable form of one .rpt.
mask_rpt() {
    grep -Ev "${VOLATILE_RE}" "$1"
}

# Run the probe over every model into <dir>.
capture() {
    local probe="$1" dir="$2"; shift 2
    local model stem rc=0
    mkdir -p "${dir}"
    for model in "$@"; do
        stem="$(basename "${model}" .inp)"
        if ! "${probe}" "${model}" "${dir}" "${stem}" >/dev/null; then
            echo "FAIL  ${stem}: probe returned non-zero" >&2
            rc=1
            continue
        fi
        # Store the masked report next to the raw one so `check` never has to
        # re-derive the mask from a possibly-different script version.
        mask_rpt "${dir}/${stem}.rpt" > "${dir}/${stem}.rpt.masked"
    done
    return ${rc}
}

# Diff <new> against <ref> for every model.
compare() {
    local ref="$1" new="$2"; shift 2
    local model stem fails=0 checked=0
    for model in "$@"; do
        stem="$(basename "${model}" .inp)"
        local bad=0
        for artifact in ".rpt.masked" ".out" ".writeback.inp"; do
            local a="${ref}/${stem}${artifact}" b="${new}/${stem}${artifact}"
            if [[ ! -f "${a}" ]]; then
                echo "FAIL  ${stem}${artifact}: no reference (run 'record' first)" >&2
                bad=1; continue
            fi
            if [[ ! -f "${b}" ]]; then
                echo "FAIL  ${stem}${artifact}: not produced by this build" >&2
                bad=1; continue
            fi
            if ! cmp -s "${a}" "${b}"; then
                echo "FAIL  ${stem}${artifact}: differs from reference" >&2
                # A few lines of context make a report regression diagnosable
                # without re-running; binary .out diffs are summarized only.
                if [[ "${artifact}" != ".out" ]]; then
                    diff -u "${a}" "${b}" | head -20 >&2 || true
                fi
                bad=1
            fi
        done
        checked=$((checked + 1))
        if [[ ${bad} -eq 0 ]]; then
            echo "ok    ${stem}"
        else
            fails=$((fails + 1))
        fi
    done
    if [[ ${fails} -gt 0 ]]; then
        echo "PARITY GATE FAILED: ${fails}/${checked} model(s) differ" >&2
        return 1
    fi
    echo "PARITY GATE PASSED: ${checked} model(s) identical"
    return 0
}

main() {
    [[ $# -ge 2 ]] || usage
    local mode="$1" probe="$2"; shift 2
    [[ -x "${probe}" ]] || { echo "not executable: ${probe}" >&2; exit 2; }

    local ref=""
    case "${mode}" in
        record|check)
            [[ $# -ge 1 ]] || usage
            ref="$1"; shift
            ;;
        selfcheck) ;;
        *) usage ;;
    esac

    # Built with a read loop rather than mapfile: macOS ships bash 3.2, which
    # has neither mapfile nor readarray.
    local models=()
    if [[ $# -gt 0 ]]; then
        models=("$@")
    else
        while IFS= read -r line; do
            [[ -n "${line}" ]] && models+=("${line}")
        done < <(default_models)
    fi
    [[ ${#models[@]} -gt 0 ]] || { echo "no models to check" >&2; exit 2; }

    case "${mode}" in
        record)
            capture "${probe}" "${ref}" "${models[@]}" || exit 1
            echo "recorded ${#models[@]} model(s) into ${ref}"
            ;;
        check)
            SCRATCH_A="$(mktemp -d)"
            capture "${probe}" "${SCRATCH_A}" "${models[@]}" || exit 1
            compare "${ref}" "${SCRATCH_A}" "${models[@]}" || exit 1
            ;;
        selfcheck)
            SCRATCH_A="$(mktemp -d)"
            SCRATCH_B="$(mktemp -d)"
            capture "${probe}" "${SCRATCH_A}" "${models[@]}" || exit 1
            capture "${probe}" "${SCRATCH_B}" "${models[@]}" || exit 1
            compare "${SCRATCH_A}" "${SCRATCH_B}" "${models[@]}" || exit 1
            ;;
    esac
}

main "$@"
