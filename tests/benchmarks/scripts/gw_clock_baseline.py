#!/usr/bin/env python3
"""Qualify GW clock telemetry against a retained, pre-change CLI/library.

All decks, dependencies, logs and comparisons remain in the output directory.
By default Bellinge is a five-minute startup slice. --full-bellinge runs only
the complete authored event, preserving its threads and output cadence.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time

from gw_clock_full_report import write_report
from soa_review_baseline import datasets, engine, sha


def report_payload(path):
    # Only wall-clock report footer metadata is excluded. All numerical rows,
    # warnings, model times and solver statistics must still match exactly.
    return b"\n".join(line for line in path.read_bytes().splitlines()
                      if not re.match(rb"\s*(Analysis begun on:|Analysis ended on:|Total elapsed time:)", line))


def clock_rows(log):
    result = {}
    for kind, body in re.findall(r"\[GW-CLOCK([^]]*)\]([^\n]+)", log):
        row = dict(re.findall(r"([\w.]+)=([^\s]+)", body))
        result.setdefault(kind or "summary", []).append(row)
    return result


def validate_clock(rows):
    summaries = rows.get("summary", [])
    if len(summaries) != 1:
        raise RuntimeError("Expected exactly one completed GW clock profile")
    summary = summaries[0]
    total = int(summary["firings"])
    if total <= 0 or sum(int(summary[k]) for k in ("direct", "macro", "tail", "rk2")) != total:
        raise RuntimeError("Clock firing origins do not close")
    closures = rows.get("-CLOSURE", [])
    if len(closures) != 3 or sum(int(c["firings"]) for c in closures) != total:
        raise RuntimeError("Closure firing counts do not close")
    for c in closures:
        if int(c["invalid_interval"]):
            raise RuntimeError("Invalid firing interval in clock profile")
        if sum(map(int, c["overfire"].split(","))) + int(c["unbounded"]) != int(c["firings"]):
            raise RuntimeError("Over-firing histogram does not close")
    sampled = sum(sum(map(int, c["zone_ratio"].split(","))) +
                  sum(int(c[k]) for k in ("g_unbounded", "u_unbounded", "both_unbounded"))
                  for c in closures)
    if sampled != int(summary["cells"]) * int(summary["rebuilds"]):
        raise RuntimeError("Stability histogram does not cover every covered cell/rebuild")
    per_cell = rows.get("-CELL", [])
    if len(per_cell) != int(summary["cells"]) or sum(int(c["firings"]) for c in per_cell) != total:
        raise RuntimeError("Per-cell counts do not close")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("base", type=Path)
    p.add_argument("candidate", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("--bellinge", type=Path)
    p.add_argument("--full-bellinge", action="store_true",
                   help="profile the complete Bellinge event first, then compare both unprofiled builds")
    p.add_argument("--repeat", type=int, default=1)
    a = p.parse_args()
    if a.bellinge:
        a.bellinge = a.bellinge.resolve()
    if a.full_bellinge and not a.bellinge:
        p.error("--full-bellinge requires --bellinge")
    if a.repeat < 1:
        p.error("--repeat must be positive")
    root = Path(__file__).resolve().parents[3]
    soa = root / "tests/benchmarks/generated/soa_review_2026-10"
    decks = [soa / "soa_sigma.inp", soa / "soa_gw_species_heat.inp"]
    decks += sorted((soa / "pipeline_parallel").glob("mixed_water_t*.inp"))
    decks += sorted((root / "tests/benchmarks/generated/gw2d_clock").glob("*.inp"))
    if a.bellinge:
        decks.append(a.bellinge.resolve())
    if a.full_bellinge:
        decks = [a.bellinge]
    base, candidate = engine(a.base), engine(a.candidate)
    if base["sha256"] == candidate["sha256"]:
        p.error("baseline and candidate libraries must differ")
    a.output.mkdir(parents=True, exist_ok=False)
    output = a.output.resolve()
    provenance = {"base": base, "candidate": candidate,
                  "base_cli_sha256": sha(a.base), "candidate_cli_sha256": sha(a.candidate),
                  "report_exclusions": ["Analysis begun on", "Analysis ended on", "Total elapsed time"],
                  "hdf5_gate": "dataset dtype, shape and exact values; container metadata excluded",
                  "inputs": {}}
    results = []
    for source in decks:
        name = ("bellinge_full" if a.full_bellinge else "bellinge_startup_5min") if source == a.bellinge else source.stem
        text = source.read_text()
        edits = []
        if source == a.bellinge and not a.full_bellinge:
            overrides = {"END_DATE": "06/29/2012", "END_TIME": "00:06:00",
                         "REPORT_START_TIME": "00:01:00", "REPORT_STEP": "00:01:00", "THREADS": "1"}
            # Replace authored keys in their original section: repeated OPTIONS
            # sections do not override every option (notably the end date).
            for key, value in overrides.items():
                text, count = re.subn(rf"^{key}\s+.*$", f"{key} {value}", text, flags=re.M)
                if not count:
                    text = text.replace("[OPTIONS]", f"[OPTIONS]\n{key} {value}", 1)
            edits.append(overrides)
        text = re.sub(r"^OUTPUT_FILE\s+.*$", "", text, flags=re.M)
        if a.full_bellinge:
            text, n = re.subn(r"^BACKEND[ \t]+.*$", "BACKEND CPU", text, flags=re.M)
            if n != 1:
                raise ValueError("Expected one authored Bellinge backend setting")
            text = text.replace("BACKEND CPU", "BACKEND CPU\nOUTPUT_FILE fields.2d.h5", 1)
            edits.append("CPU backend; local HDF5 path; original full event, reporting and thread count")
        else:
            text += "\n[2D_OPTIONS]\nBACKEND CPU\nREPORT_2D YES\nREPORT_2D_VARIABLES ALL\nOUTPUT_FILE fields.2d.h5\n"
            edits.append("CPU backend; all HDF5 fields; output paths confined to run directory")
        dependencies = [path for path in source.parent.iterdir()
                        if path.is_file() and path.suffix.lower() in {".dat", ".2dm", ".heat"}]
        provenance["inputs"][name] = {"path": str(source), "sha256": sha(source), "edits": edits,
                                     "dependencies": {str(path): sha(path) for path in dependencies}}
        (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
        for repeat in range(a.repeat):
            runs = {}
            if a.full_bellinge:
                write_report(output, repeat)
            sides = [("base", a.base, False), ("candidate", a.candidate, False)]
            if repeat % 2:
                sides.reverse()
            sides.append(("profiled", a.candidate, True))
            if a.full_bellinge:
                sides.insert(0, sides.pop())  # produce the outstanding physical profile first
            for side, cli, profiled in sides:
                dest = output / name / str(repeat) / side
                dest.mkdir(parents=True)
                deck = dest / "model.inp"
                deck.write_text(text)
                for dependency in dependencies:
                    (dest / dependency.name).symlink_to(dependency.resolve())
                env = os.environ.copy()
                for key in ("OPENSWMM_PERF", "OPENSWMM_GW_CLOCK_CELLS", "DYLD_INSERT_LIBRARIES"):
                    env.pop(key, None)
                if profiled:
                    env.update(OPENSWMM_PERF="1", OPENSWMM_GW_CLOCK_CELLS="1")
                started = time.perf_counter()
                print(f"Starting {name} #{repeat} {side}: {dest}", flush=True)
                with (dest / "console.log").open("w") as log:
                    run = subprocess.run([str(cli.resolve()), str(deck), "model.rpt", "model.out"],
                                         cwd=dest, env=env, stdout=log, stderr=subprocess.STDOUT,
                                         timeout=None if a.full_bellinge else 1800)
                wall = time.perf_counter() - started
                if run.returncode:
                    raise RuntimeError(f"Run failed: {dest}/console.log")
                log = (dest / "console.log").read_text()
                if "[PERF-PROCESS-INVALID]" in log:
                    raise RuntimeError(f"Invalid profiling attribution: {dest}")
                clocks = clock_rows(log)
                if profiled:
                    validate_clock(clocks)
                    expected = 172680 if a.full_bellinge else 300  # 00:01 on June 29 to 23:59 on June 30
                    if source == a.bellinge and abs(float(clocks["summary"][0]["seconds"]) - expected) > 1e-8:
                        raise RuntimeError(f"Bellinge did not simulate the expected {expected} seconds")
                elif clocks:
                    raise RuntimeError(f"Disabled telemetry emitted output: {dest}")
                phases = {k: dict(re.findall(r"([\w.]+)=([^\s]+)", body))
                          for k, body in re.findall(r"\[PERF-([^]]+)\]([^\n]+)", log)}
                fields = datasets(dest / "fields.2d.h5")
                if not fields:
                    raise RuntimeError(f"No HDF5 datasets: {dest}")
                runs[side] = {"wall_seconds": wall, "out_sha256": sha(dest / "model.out"),
                              "datasets": fields, "phases": phases, "clock": clocks}
                if a.full_bellinge:
                    (dest / "measurement.json").write_text(json.dumps(runs[side], indent=2) + "\n")
                    write_report(output, repeat)
                    print(f"Completed {side} in {wall:.1f} s; exact comparison follows all runs", flush=True)
            base_dir = output / name / str(repeat) / "base"
            for side in ("candidate", "profiled"):
                dest = output / name / str(repeat) / side
                for key in ("out_sha256", "datasets"):
                    if runs[side][key] != runs["base"][key]:
                        raise RuntimeError(f"{key} differs: {dest}")
                if report_payload(dest / "model.rpt") != report_payload(base_dir / "model.rpt"):
                    raise RuntimeError(f"Report numerical payload differs: {dest}")
            results.append({"deck": name, "repeat": repeat, "exact_output": True, "runs": runs})
            (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
            if a.full_bellinge:
                write_report(output, repeat)
            print(f"{name} #{repeat}: exact .out, report payload, HDF5; clock counters close", flush=True)


if __name__ == "__main__":
    main()
