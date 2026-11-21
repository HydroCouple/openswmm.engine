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
"""Compare two epaswmm5_qa score files (swashes / transitions) for given solvers.

    python3 fv_perf_compare_scores.py BEFORE.json AFTER.json --solvers 1d-fv [--md OUT.md]

Per (case, solver): verdict before/after, the primary metric (l1_h, or the
suite's own key metric when l1_h is absent), mass %, wall. Flags every
PASS→FAIL and every metric that worsened by more than --tol (relative,
default 0.10). Exit 1 on any flag — the Phase 2 analytic gate of
plans/FV1D_CLOSURE_KERNEL_PERF_PLAN_2026-09-11.md §2c.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

PRIMARY = ("l1_h", "front_speed_err", "head_hold_err", "head_steady_err", "linf_h")


def cells(path: Path, solvers: set[str]) -> dict[tuple[str, str], dict]:
    d = json.loads(path.read_text(encoding="utf-8"))
    out = {}
    for c in d["cells"]:
        if c.get("solver") in solvers:
            out[(c["case"], c["solver"])] = c
    return out


def primary(c: dict) -> tuple[str, float | None]:
    m = c.get("metrics") or {}
    for k in PRIMARY:
        if k in m and m[k] is not None:
            try:
                return k, float(m[k])
            except (TypeError, ValueError):
                pass
    return "—", None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("before", type=Path)
    ap.add_argument("after", type=Path)
    ap.add_argument("--solvers", default="1d-fv")
    ap.add_argument("--tol", type=float, default=0.10)
    ap.add_argument("--md", type=Path, default=None)
    args = ap.parse_args()
    solvers = {s.strip() for s in args.solvers.split(",") if s.strip()}

    b = cells(args.before, solvers)
    a = cells(args.after, solvers)
    keys = sorted(k for k in a if k in b)
    if not keys:
        print("no common cells", file=sys.stderr)
        return 2

    rows = ["| case | solver | verdict before → after | metric | before | after | ratio | mass % b/a | flag |",
            "|---|---|---|---|---:|---:|---:|---:|---|"]
    flags = 0
    for case, solver in keys:
        cb, ca = b[(case, solver)], a[(case, solver)]
        vb, va = cb.get("verdict"), ca.get("verdict")
        kb, mb = primary(cb)
        ka, ma = primary(ca)
        flag = []
        if vb in ("PASS", "BASE-PASS") and va not in ("PASS", "BASE-PASS"):
            flag.append("PASS→FAIL")
        ratio = None
        if mb is not None and ma is not None and mb > 0:
            ratio = ma / mb
            if ratio > 1.0 + args.tol and va not in ("PASS", "BASE-PASS", "XFAIL"):
                flag.append(f"metric ×{ratio:.2f}")
            elif ratio > 1.0 + args.tol:
                flag.append(f"metric ×{ratio:.2f} (still {va})")
        if flag:
            flags += 1
        rows.append(f"| {case} | {solver} | {vb} → {va} | {ka} | "
                    f"{'—' if mb is None else f'{mb:.4g}'} | {'—' if ma is None else f'{ma:.4g}'} | "
                    f"{'—' if ratio is None else f'{ratio:.2f}'} | "
                    f"{cb.get('mass_pct', '—')}/{ca.get('mass_pct', '—')} | {', '.join(flag)} |")
    text = "\n".join(rows) + "\n"
    print(text)
    print(f"{len(keys)} cells compared, {flags} flagged")
    if args.md:
        args.md.write_text(f"# score compare\n\nbefore: `{args.before}`  \nafter: `{args.after}`\n\n"
                           + text + f"\n{len(keys)} cells compared, {flags} flagged\n",
                           encoding="utf-8")
    return 1 if flags else 0


if __name__ == "__main__":
    raise SystemExit(main())
