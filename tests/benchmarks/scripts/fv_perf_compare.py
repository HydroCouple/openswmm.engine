#!/usr/bin/env python3
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
"""Compare two fv_perf_baseline.py runs: the Tier A hash gate plus wall deltas.

    python3 fv_perf_compare.py BASE/results.json NEW/results.json [--md OUT.md]

For every (deck, config) present in both: whether sha256(.out) matches (the
Tier A gate — a change that claims bit-identity must match on every row),
the wall-clock ratio new/base, and the closure-call and substep counters side
by side. Exit 1 if any hash differs, so it can gate a script.

Wall clocks are only comparable when both runs saw the same host load; the
hashes and counters are load-independent, which is why this prints them first.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def load(path: Path) -> dict[tuple[str, str], dict]:
    data = json.loads(path.read_text(encoding="utf-8"))
    return {(r["deck"], r["config"]): r for r in data["results"]}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base", type=Path)
    ap.add_argument("new", type=Path)
    ap.add_argument("--md", type=Path, default=None, help="also write a markdown table")
    args = ap.parse_args()

    base = load(args.base)
    new = load(args.new)
    keys = [k for k in new if k in base]
    if not keys:
        print("no common (deck, config) rows", file=sys.stderr)
        return 2

    rows: list[str] = []
    rows.append("| deck | config | hash | wall base (s) | wall new (s) | new/base | "
                "substeps b/n | invert b/n | area b/n | width b/n | i1 b/n |")
    rows.append("|---|---|:-:|---:|---:|---:|---:|---:|---:|---:|---:|")
    mismatches = 0
    for deck, cfg in keys:
        b, n = base[(deck, cfg)], new[(deck, cfg)]
        same = (b.get("out_sha256") == n.get("out_sha256")) and bool(b.get("out_sha256"))
        if not same:
            mismatches += 1
        pb, pn = b.get("perf") or {}, n.get("perf") or {}
        g = lambda p, k: int(p.get(k, 0.0))  # noqa: E731
        ratio = (n["wall_best"] / b["wall_best"]) if b.get("wall_best") else float("nan")
        rows.append(
            f"| {deck} | {cfg} | {'same' if same else '**DIFF**'} | "
            f"{b.get('wall_best', float('nan')):.2f} | {n.get('wall_best', float('nan')):.2f} | "
            f"{ratio:.2f} | {g(pb, 'n.substep')}/{g(pn, 'n.substep')} | "
            f"{g(pb, 'n.invert')}/{g(pn, 'n.invert')} | {g(pb, 'n.area')}/{g(pn, 'n.area')} | "
            f"{g(pb, 'n.width')}/{g(pn, 'n.width')} | {g(pb, 'n.i1')}/{g(pn, 'n.i1')} |")

    text = "\n".join(rows) + "\n"
    print(text)
    print(f"{len(keys)} rows compared, {mismatches} hash mismatches")
    if args.md:
        args.md.write_text(
            f"# fv_perf compare\n\nbase: `{args.base}`  \nnew: `{args.new}`\n\n" + text +
            f"\n{len(keys)} rows compared, {mismatches} hash mismatches\n",
            encoding="utf-8")
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())
