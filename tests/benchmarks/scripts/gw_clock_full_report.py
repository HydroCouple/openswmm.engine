#!/usr/bin/env python3
"""Render the measured full Bellinge profile, independently of parity completion."""
import argparse
import json
from pathlib import Path


def write_report(output, repeat=0, status=None):
    rowdir = output / "bellinge_full" / str(repeat)
    profile = rowdir / "profiled" / "measurement.json"
    final = output / "results.json"
    evidence = []
    if final.exists():
        evidence = [r for r in json.loads(final.read_text())
                    if r["deck"] == "bellinge_full" and r["repeat"] == repeat and r["exact_output"]]
    lines = ["# Bellinge: full-event shared-clock baseline", "",
             "Configured duration: 172,680 s; original thread count and report cadence. "
             "See [input and binary hashes](provenance.json).", ""]
    if status:
        lines += [status, ""]
    if profile.exists():
        p = json.loads(profile.read_text())
        s = p["clock"]["summary"][0]
        if abs(float(s["seconds"]) - 172680) > 1e-8:
            raise ValueError("Profile is not the complete Bellinge event")
        lines += [f"Profile: {int(s['cells']):,} cells; {int(s['firings']):,} physical firings "
                  f"over {float(s['seconds']):,.0f} simulated seconds. "
                  f"Per-cell firings/hour (min / mean / max): "
                  f"{float(s['min_per_hour']):.3f} / {float(s['mean_per_hour']):.3f} / "
                  f"{float(s['max_per_hour']):.3f}.", "",
                  "Origin: " + "; ".join(f"{k} {int(s[k]):,}" for k in ("direct", "macro", "tail", "rk2")) + ".", "",
                  "| Closure | Firings | Stability / elapsed interval | Saturated / column bound | "
                  "Unbounded (overfire; saturated / column / both) |", "|---|---:|---|---|---|"]
        totals = [0] * 8
        for c in p["clock"]["-CLOSURE"]:
            totals = [a + b for a, b in zip(totals, map(int, c["overfire"].split(",")), strict=True)]
            lines.append(f"| {c['closure']} | {int(c['firings']):,} | {c['overfire']} | {c['zone_ratio']} | "
                         f"{c['unbounded']}; {c['g_unbounded']} / {c['u_unbounded']} / {c['both_unbounded']} |")
        lines += ["", "Stability / elapsed bins: <1, 1–2, 2–8, 8–32, 32–128, 128–512, 512–2048, ≥2048. "
                  "Zone bins: <1/16, 1/16–1/4, 1/4–1, 1–4, 4–16, ≥16. "
                  "Bins are left inclusive. These are observed counts, not achievable speedups.", "",
                  "| GW process | Calls | Instrumented seconds |", "|---|---:|---:|"]
        finite = sum(totals)
        if finite:
            labels = ["<1", "[1,2)", "[2,8)", "[8,32)", "[32,128)", "[128,512)", "[512,2048)", "≥2048"]
            median = next(label for i, label in enumerate(labels) if sum(totals[:i+1]) * 2 >= finite)
            lines[-2:-2] = [f"Among {finite:,} firings with finite stability bounds, the median "
                            f"stability/elapsed ratio falls in {median}; "
                            f"{totals[-1] / finite:.2%} have a ratio ≥2048.", ""]
        phases = p["phases"]["GW2D"]
        for k, v in phases.items():
            if not k.startswith("n."):
                lines.append(f"| {k} | {int(phases['n.' + k]):,} | {float(v):.3f} |")
        lines += ["", "Nested timers charge their own work on each thread. Parallel work means their sum is not elapsed wall time.", ""]
    else:
        lines += ["The full-event profile has not yet completed. The five-minute slice is separate evidence.", ""]
    if evidence:
        r = evidence[0]["runs"]
        lines += ["**Parity passed:** exact binary output, numerical report contents and every HDF5 dataset "
                  "for baseline, candidate with profiling off and candidate with profiling on.", "",
                  f"Wall seconds, one run per build/mode: baseline {r['base']['wall_seconds']:.1f}, "
                  f"candidate {r['candidate']['wall_seconds']:.1f}, profiled {r['profiled']['wall_seconds']:.1f}."]
    else:
        lines += ["**Full-event parity: unverified.** The runner writes results.json only after all three "
                  "runs have completed and all comparisons have passed."]
    name = output / ("RESULTS.md" if repeat == 0 else f"RESULTS_{repeat}.md")
    pending = name.with_suffix(".md.part")
    pending.write_text("\n".join(lines) + "\n")
    pending.replace(name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--repeat-index", type=int, default=0)
    args = parser.parse_args()
    write_report(args.output, args.repeat_index)
