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
"""Tables from a runoff_phase0.sh results folder.

Reads ``timing.tsv``, ``legacy.tsv``, ``identity.txt``, ``perf/*.perf`` and
``counters/*.perf`` and prints Markdown:

* identity: the byte comparison of every deck;
* wall time per deck and thread count (median of the reps, spread), with the
  legacy CLI's wall time beside the 1D decks;
* the [PERF-HYD] section self times of the runoff substep as shares of
  ``runoff_step`` (its own self time is the unattributed remainder);
* the [PERF-2D] / [PERF-GW2D] phase self times of the 2D decks;
* the event counters as per-event ratios: ODE steps and rejected trials per
  call, ``pow`` calls per subarea-step, the branch shares of every
  infiltration method, Newton iterations per σ-column solve, and how often
  the GW2D sweeps ran serially.

Usage::

    python3 runoff_phase0_report.py <results-dir> > report.md

stdlib only.
"""

from __future__ import annotations

import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

KV = re.compile(r"(\S+?)=(\S+)")


def parse_perf_line(line: str) -> dict[str, float]:
    """``[PERF-X] a=1 n.a=2 ...`` -> {"a": 1.0, "n.a": 2.0}."""
    body = line.split("]", 1)[1] if "]" in line else line
    out: dict[str, float] = {}
    for k, v in KV.findall(body):
        try:
            out[k] = float(v)
        except ValueError:
            pass
    return out


def read_perf_file(path: Path) -> dict[str, dict[str, float]]:
    groups: dict[str, dict[str, float]] = {}
    for line in path.read_text().splitlines():
        m = re.match(r"\[PERF-([A-Z0-9-]+)\]", line)
        if m:
            groups.setdefault(m.group(1), {}).update(parse_perf_line(line))
    return groups


def median_spread(xs: list[float]) -> tuple[float, float]:
    med = statistics.median(xs)
    return med, (max(xs) - min(xs)) if len(xs) > 1 else 0.0


def fmt(x: float, nd: int = 2) -> str:
    return f"{x:.{nd}f}"


def pct(num: float, den: float) -> str:
    return f"{100.0 * num / den:.1f}%" if den > 0 else "-"


def ratio(num: float, den: float, nd: int = 2) -> str:
    return fmt(num / den, nd) if den > 0 else "-"


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.split("\n")[0], file=sys.stderr)
        print("usage: runoff_phase0_report.py <results-dir>", file=sys.stderr)
        return 2
    res = Path(argv[1])
    out: list[str] = []

    # ---- identity ---------------------------------------------------------
    ident = res / "identity.txt"
    if ident.exists():
        out.append("## Identity (tip vs candidate .out, 8-byte header skipped)\n")
        lines = ident.read_text().splitlines()
        bad = [l for l in lines if "IDENTICAL" not in l]
        out.append(f"{len(lines) - len(bad)} identical, {len(bad)} not:\n")
        out.extend(f"- {l}" for l in bad)
        out.append("")

    # ---- timing -----------------------------------------------------------
    legacy: dict[str, float] = {}
    leg = res / "legacy.tsv"
    if leg.exists():
        for row in leg.read_text().splitlines()[1:]:
            f = row.split("\t")
            if len(f) >= 2 and f[1] not in ("-", ""):
                legacy[f[0]] = float(f[1])

    walls: dict[tuple[str, str], list[float]] = defaultdict(list)
    peaks: dict[tuple[str, str], list[float]] = defaultdict(list)
    timing = res / "timing.tsv"
    if timing.exists():
        for row in timing.read_text().splitlines()[1:]:
            f = row.split("\t")
            if len(f) < 6 or f[3] in ("-", ""):
                continue
            walls[(f[0], f[1])].append(float(f[3]))
            if f[4] not in ("-", ""):
                peaks[(f[0], f[1])].append(float(f[4]))
        out.append("## Wall time (OPENSWMM_PERF=1, median of reps; spread = max - min)\n")
        out.append("| deck | threads | wall s | spread s | peak MB | legacy s (1 thr) | speedup vs 1 thr |")
        out.append("|---|---|---|---|---|---|---|")
        base: dict[str, float] = {}
        for (deck, thr), xs in sorted(walls.items(), key=lambda kv: (kv[0][0], int(kv[0][1]))):
            med, spread = median_spread(xs)
            if thr == "1":
                base[deck] = med
            pk = statistics.median(peaks[(deck, thr)]) if peaks[(deck, thr)] else float("nan")
            legs = fmt(legacy[deck]) if deck in legacy and thr == "1" else ""
            sp = ratio(base.get(deck, 0.0), med) + "x" if deck in base and thr != "1" else ""
            out.append(f"| {deck} | {thr} | {fmt(med)} | {fmt(spread)} | {pk:.0f} | {legs} | {sp} |")
        out.append("")

    # ---- section self times ---------------------------------------------
    perf_dir = res / "perf"
    sections: dict[tuple[str, str], list[dict[str, float]]] = defaultdict(list)
    if perf_dir.exists():
        for p in sorted(perf_dir.glob("*.perf")):
            m = re.match(r"(.+)_t(\d+)_r\d+\.perf$", p.name)
            if not m:
                continue
            sections[(m.group(1), m.group(2))].append(read_perf_file(p))

    hyd_keys = ["runoff_step", "runoff_prep", "runoff_weather", "runoff_snow", "runoff",
                "runoff_post", "runoff_lid_io", "lid", "runoff_gw_io", "groundwater",
                "runoff_massbal", "subcatch_snow"]
    rows = []
    for (deck, thr), runs in sorted(sections.items(), key=lambda kv: (kv[0][0], int(kv[0][1]))):
        hyd = [r.get("HYD", {}) for r in runs if "HYD" in r]
        if not hyd or not any(h.get("runoff_step", 0) > 0 for h in hyd):
            continue
        med = {k: statistics.median([h.get(k, 0.0) for h in hyd]) for k in hyd_keys}
        total = sum(med.values())  # every section + kernel self time = the substep's wall
        if total < 0.05:  # a mesh-only deck: the substep runs but has nothing to do
            continue
        rows.append((deck, thr, med, total))
    if rows:
        out.append("## Runoff substep: self time per section ([PERF-HYD], 1D decks)\n")
        out.append("Shares of the substep's wall time (the sum of every self time). "
                   "`runoff_step` is the unattributed remainder of the body; `runoff` / `lid` / "
                   "`groundwater` / `subcatch_snow` are the kernels' own timers.\n")
        out.append("| deck | thr | substep total s | " + " | ".join(hyd_keys) + " |")
        out.append("|---|---|---|" + "---|" * len(hyd_keys))
        for deck, thr, med, total in rows:
            out.append(f"| {deck} | {thr} | {fmt(total)} | "
                       + " | ".join(pct(med[k], total) for k in hyd_keys) + " |")
        out.append("")

    # ---- 2D phases --------------------------------------------------------
    rows2d = []
    for (deck, thr), runs in sorted(sections.items(), key=lambda kv: (kv[0][0], int(kv[0][1]))):
        if not deck.startswith("bench2d"):
            continue
        keys: dict[str, list[float]] = defaultdict(list)
        for r in runs:
            for grp in ("2D", "GW2D"):
                for k, v in r.get(grp, {}).items():
                    if not k.startswith("n."):
                        keys[f"{grp}.{k}"].append(v)
        med = {k: statistics.median(v) for k, v in keys.items() if statistics.median(v) > 0}
        if med:
            rows2d.append((deck, thr, med))
    if rows2d:
        out.append("## 2D decks: phase self times ([PERF-2D] / [PERF-GW2D], seconds, median)\n")
        allk = sorted({k for _, _, m in rows2d for k in m})
        out.append("| deck | thr | " + " | ".join(allk) + " |")
        out.append("|---|---|" + "---|" * len(allk))
        for deck, thr, med in rows2d:
            out.append(f"| {deck} | {thr} | " + " | ".join(fmt(med.get(k, 0.0), 3) for k in allk) + " |")
        out.append("")

    # ---- counters ---------------------------------------------------------
    cdir = res / "counters"
    if cdir.exists():
        out.append("## Event counters (profile build, one thread)\n")
        out.append("| deck | subarea-steps | ODE calls / subarea | steps / ODE call | rejects / ODE call "
                   "| ODE calls with 1 step | pow / subarea-step | below storage | loss shortcut | linear |")
        out.append("|---|---|---|---|---|---|---|---|---|---|")
        infil_rows = []
        sigma_rows = []
        for p in sorted(cdir.glob("*.perf")):
            deck = p.stem
            c = read_perf_file(p).get("PROCESS-COUNTS", {})
            sub = c.get("runoff.subarea", 0.0)
            calls = c.get("runoff.ode.calls", 0.0)
            if sub > 0:
                pows = c.get("runoff.ode.pow", 0.0) + c.get("runoff.pow_outflow", 0.0)
                out.append(
                    f"| {deck} | {sub:.0f} | {ratio(calls, sub)} | {ratio(c.get('runoff.ode.steps', 0), calls)} "
                    f"| {ratio(c.get('runoff.ode.rejects', 0), calls)} | {pct(c.get('runoff.ode.hist.1', 0), calls)} "
                    f"| {ratio(pows, sub)} | {pct(c.get('runoff.below_store', 0), sub)} "
                    f"| {pct(c.get('runoff.loss_shortcut', 0), sub)} | {pct(c.get('runoff.linear', 0), sub)} |")
            # infiltration branches
            for method, keys in (
                ("horton", ["special", "wet", "newton", "newton_iter", "dry"]),
                ("modhorton", ["special", "wet", "dry"]),
                ("ga", ["sat", "unsat_dry", "unsat_lowrain", "unsat_wet", "f2_explicit", "f2_newton",
                        "f2_newton_iter", "f2_exhausted"]),
                ("cn", ["rain", "pond", "dry"]),
            ):
                vals = {k: c.get(f"infil.{method}.{k}", 0.0) for k in keys}
                if any(vals.values()):
                    infil_rows.append((deck, method, vals))
            if c.get("sigma.solves", 0) > 0 or c.get("gw2d.cells.parallel", 0) + c.get(
                    "gw2d.cells.serial_profile", 0) + c.get("gw2d.cells.serial_small", 0) > 0:
                sigma_rows.append((deck, c))
        out.append("")
        if infil_rows:
            out.append("### Infiltration branches (calls per element-step; iterations where counted)\n")
            out.append("| deck | method | branch counts |")
            out.append("|---|---|---|")
            for deck, method, vals in infil_rows:
                out.append(f"| {deck} | {method} | " + ", ".join(f"{k}={v:.0f}" for k, v in vals.items()) + " |")
            out.append("")
        if sigma_rows:
            out.append("### σ column and GW2D sweeps\n")
            out.append("| deck | solves | Newton iter / solve | line-search halvings / solve | substep-halved firings "
                       "| failed firings | cell sweeps parallel / serial(profile) / serial(small) | face loops parallel / serial |")
            out.append("|---|---|---|---|---|---|---|---|")
            for deck, c in sigma_rows:
                s = c.get("sigma.solves", 0.0)
                out.append(
                    f"| {deck} | {s:.0f} | {ratio(c.get('sigma.newton_iter', 0), s)} "
                    f"| {ratio(c.get('sigma.linesearch_halvings', 0), s)} | {c.get('sigma.substep_halved', 0):.0f} "
                    f"| {c.get('sigma.column_failed', 0):.0f} "
                    f"| {c.get('gw2d.cells.parallel', 0):.0f} / {c.get('gw2d.cells.serial_profile', 0):.0f} / {c.get('gw2d.cells.serial_small', 0):.0f} "
                    f"| {c.get('gw2d.faces.parallel', 0):.0f} / {c.get('gw2d.faces.serial', 0):.0f} |")
            out.append("")

    print("\n".join(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
