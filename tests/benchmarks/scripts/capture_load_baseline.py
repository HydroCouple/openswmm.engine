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
"""Capture load-benchmark medians + phase-timer breakdown into a markdown doc.

Plan Phase 0.3, "Baseline capture". Emits
``tests/benchmarks/generated/LOAD_<label>_<date>.md``, the document every later
commit in the optimization program measures against.

Two things this does that a bare ``bench_model_load`` run does not:

* **One model per process.** ``getrusage`` reports a process-wide high-water
  mark, so peak RSS is only attributable to a model if that model is the only
  one the process loaded.
* **A separate ``OPENSWMM_PERF`` pass** per model to collect the
  ``[PERF-LOAD]`` phase split, which the Google Benchmark JSON does not carry.

Usage::

    python3 capture_load_baseline.py --bench <bench_model_load> \\
        [--corpus DIR] [--out FILE] [--label baseline] [--model NAME ...]

stdlib only.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

CUTS = ("open", "open_init", "full")

#: `key=value` pairs on the engine's [PERF-LOAD] stderr line.
_PERF_RE = re.compile(r"([A-Za-z0-9_.]+)=([0-9.]+)")


def _median_rows(bench_json: dict) -> dict:
    """Maps cut name -> (median_ms, peak_rss_MB) from one benchmark run."""
    out: dict[str, tuple[float, float]] = {}
    for b in bench_json.get("benchmarks", []):
        name = b.get("name", "")
        if not name.endswith("_median"):
            continue
        # BM_Load/<model>/<cut>/min_time:.../repeats:N/real_time_median
        parts = name.split("/")
        if len(parts) < 3:
            continue
        cut = parts[2]
        out[cut] = (float(b.get("real_time", 0.0)),
                    float(b.get("peak_rss_MB", 0.0)))
    return out


def run_bench(bench: Path, corpus: Path, model: str,
              repetitions: int) -> dict[str, tuple[float, float]]:
    """Runs all three cuts for one model in a dedicated process."""
    # Repetitions travel by env var, not --benchmark_repetitions: Google
    # Benchmark ignores that flag for benchmarks that registered an explicit
    # count, which these do.
    env = dict(os.environ, OPENSWMM_BENCH_CORPUS=str(corpus),
               OPENSWMM_BENCH_REPS=str(repetitions))
    # The filter is anchored on the model segment so grid_10k does not also
    # match grid_100k.
    cmd = [str(bench),
           f"--benchmark_filter=BM_Load/{re.escape(model)}/",
           "--benchmark_format=json"]
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"    bench failed: {proc.stderr.strip()[:400]}", file=sys.stderr)
        return {}
    try:
        return _median_rows(json.loads(proc.stdout))
    except json.JSONDecodeError:
        print(f"    unparseable benchmark output for {model}", file=sys.stderr)
        return {}


def run_perf(bench: Path, corpus: Path, model: str) -> dict[str, float]:
    """One OPENSWMM_PERF run of the full cut; returns the phase split."""
    env = dict(os.environ, OPENSWMM_BENCH_CORPUS=str(corpus),
               OPENSWMM_PERF="1", OPENSWMM_BENCH_REPS="2")
    cmd = [str(bench),
           f"--benchmark_filter=BM_Load/{re.escape(model)}/full",
           "--benchmark_min_time=1x"]
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    # Each close() emits one line; the last is a complete, self-consistent
    # sample (the accumulators are reset by every open()).
    last = ""
    for line in proc.stderr.splitlines():
        if line.startswith("[PERF-LOAD]"):
            last = line
    return {k: float(v) for k, v in _PERF_RE.findall(last)} if last else {}


def main(argv: list[str] | None = None) -> int:
    here = Path(__file__).resolve()
    bench_dir = here.parents[1]

    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bench", required=True, help="bench_model_load binary")
    ap.add_argument("--corpus", default=str(bench_dir / "generated"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--label", default="BASELINE",
                    help="BASELINE, or e.g. RESULTS_phase1.1")
    ap.add_argument("--model", action="append", default=None)
    ap.add_argument("--repetitions", type=int, default=5)
    args = ap.parse_args(argv)

    bench = Path(args.bench).resolve()
    corpus = Path(args.corpus).resolve()
    if not bench.is_file():
        print(f"no such benchmark binary: {bench}", file=sys.stderr)
        return 2
    if not corpus.is_dir():
        print(f"no such corpus dir: {corpus}", file=sys.stderr)
        return 2

    models = args.model or sorted(p.stem for p in corpus.glob("*.inp"))
    if not models:
        print(f"no .inp models in {corpus}", file=sys.stderr)
        return 2

    date = _dt.date.today().isoformat()
    out_path = Path(args.out) if args.out else (
        corpus / f"LOAD_{args.label}_{date}.md")

    results: dict[str, dict] = {}
    for i, m in enumerate(models, 1):
        print(f"[{i}/{len(models)}] {m}", flush=True)
        cuts = run_bench(bench, corpus, m, args.repetitions)
        perf = run_perf(bench, corpus, m)
        results[m] = {"cuts": cuts, "perf": perf}
        if cuts:
            shown = "  ".join(
                f"{c}={cuts[c][0]:.1f}ms" for c in CUTS if c in cuts)
            print(f"        {shown}", flush=True)

    # ---- report ----------------------------------------------------------
    lines: list[str] = []
    lines.append(f"# Model load benchmark — {args.label}")
    lines.append("")
    lines.append(f"**Date:** {date}  ")
    lines.append(f"**Host:** {platform.platform()}  ")
    lines.append(f"**CPU:** {platform.processor() or platform.machine()}  ")
    lines.append(f"**Benchmark:** `{bench}`  ")
    lines.append(f"**Corpus:** `{corpus}`  ")
    lines.append(f"**Repetitions:** {args.repetitions} (medians reported)")
    lines.append("")
    lines.append("Times are milliseconds, medians of the reported repetitions.")
    lines.append("`open` is open+close; `open_init` adds initialize();")
    lines.append("`full` adds start()+end()+report(). No routing steps are run.")
    lines.append("Peak RSS is the process high-water mark, one process per model.")
    lines.append("")
    lines.append("| Model | open (ms) | open_init (ms) | full (ms) | peak RSS (MB) |")
    lines.append("|---|---:|---:|---:|---:|")
    for m in models:
        c = results[m]["cuts"]
        def cell(cut: str) -> str:
            return f"{c[cut][0]:.1f}" if cut in c else "—"
        rss = max((c[k][1] for k in c), default=0.0)
        lines.append(f"| `{m}` | {cell('open')} | {cell('open_init')} | "
                     f"{cell('full')} | {rss:.0f} |")
    lines.append("")

    lines.append("## Phase-timer breakdown (`OPENSWMM_PERF=1`, full cut, seconds)")
    lines.append("")
    cols = ["open.read", "open.resolve", "open.validate", "open.prescan2d",
            "res.extfiles", "res.tables", "res.transects", "res.xsect",
            "res.shrink", "init.state", "init.hydraulics", "init.hydrology",
            "init.quality", "init.geometry", "start.iface", "start.plugins"]
    lines.append("| Model | " + " | ".join(cols) + " |")
    lines.append("|---" * (len(cols) + 1) + "|")
    for m in models:
        p = results[m]["perf"]
        row = " | ".join(f"{p.get(c, 0.0):.3f}" for c in cols)
        lines.append(f"| `{m}` | {row} |")
    lines.append("")
    lines.append("Phase timers come from a separate single-iteration run, so "
                 "they will not sum exactly to the medians above.")
    lines.append("")

    out_path.write_text("\n".join(lines) + "\n")
    print(f"\nwrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
