#!/usr/bin/env python3
"""Measure paired clock modes without changing event, threads or report cadence.

Each mode has profiled and unprofiled runs, checked for exact numerical parity.
Completed measurements are resumable only with identical inputs and binaries.
Performance targets are recorded as pass/fail observations, not accuracy waivers.

--window START END (each "MM/DD/YYYY HH:MM:SS") runs a cut of the event instead of the whole of
it: the deck's start, report start and end are rewritten and nothing else. A window that starts
after the event's start has no antecedent wetting, so its numbers describe that regime (a storm
burst, a rain-free stretch), not the full event. Each run also records the engine process's CPU
time and the load average around it, so a comparison made on a shared machine can be judged.
--settle-load MAX waits, before each run and for at most --settle-wait seconds, until the 1-minute
load average is at or below MAX; the wait and whether the load settled are recorded with the run.
"""
import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import resource
import statistics
import subprocess
import time

import h5py
import numpy as np

from gw_clock_accuracy import residuals
from gw_clock_baseline import clock_rows, report_payload, validate_clock
from soa_review_baseline import datasets, engine, sha

ROOT = Path(__file__).resolve().parents[3]
FIXTURES = ROOT / "tests/benchmarks/generated/soa_review_2026-10"


def finite(value, path, found):
    """Return `value` with NaN/inf floats replaced by None, appending each one's path to `found`."""
    if isinstance(value, float) and not math.isfinite(value):
        found.append(f"{path}: {value}")
        return None
    if isinstance(value, dict):
        return {key: finite(item, f"{path}/{key}", found) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [finite(item, f"{path}/{index}", found) for index, item in enumerate(value)]
    return value


def save(path, value):
    # A non-finite budget is a result to report, not a reason to lose the run's results.
    found = []
    value = finite(value, "", found)
    if found:
        value["nonfinite"] = found
    pending = path.with_suffix(path.suffix + ".part")
    pending.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")
    pending.replace(path)


def prepare(source, mode, window=None):
    text = source.read_text()
    if window:
        for key, value in (("START_DATE", window[0].strftime("%m/%d/%Y")),
                           ("START_TIME", window[0].strftime("%H:%M:%S")),
                           ("REPORT_START_DATE", window[0].strftime("%m/%d/%Y")),
                           ("REPORT_START_TIME", window[0].strftime("%H:%M:%S")),
                           ("END_DATE", window[1].strftime("%m/%d/%Y")),
                           ("END_TIME", window[1].strftime("%H:%M:%S"))):
            text, count = re.subn(rf"^(\s*{key}\s+)[^\n]*", rf"\g<1>{value}", text, flags=re.M)
            assert count == 1, key
    text = re.sub(r"^\s*(?:GW_CLOCK|BACKEND|OUTPUT_FILE)\s+[^\n]*\n", "", text, flags=re.M)
    text = text.replace("[2D_OPTIONS]", "[2D_OPTIONS]\nBACKEND CPU\nOUTPUT_FILE fields.2d.h5", 1)
    if "[2D_AQUIFER_OPTIONS]" in text:
        text = text.replace("[2D_AQUIFER_OPTIONS]", f"[2D_AQUIFER_OPTIONS]\nGW_CLOCK {mode}", 1)
    else:
        text += f"\n[2D_AQUIFER_OPTIONS]\nGW_CLOCK {mode}\n"
    # Dependencies retain their original basenames and are linked beside the deck.
    dependencies = list(source.parent.glob("*.dat")) + list(source.parent.glob("*.2dm"))
    dependencies += list(source.parent.glob("*.heat"))
    return text, dependencies


def settle(maximum, wait):
    """Wait until the 1-minute load average is <= maximum, at most `wait` s; return (s, settled)."""
    started = time.perf_counter()
    while os.getloadavg()[0] > maximum and time.perf_counter() - started < wait:
        time.sleep(15)
    return time.perf_counter() - started, os.getloadavg()[0] <= maximum


def measure(cli, dest, profiled, expected_seconds, slack=1e-8, quiet=None):
    env = dict(os.environ)
    for key in list(env):
        if key.startswith(("GW_STUDY_", "GW_CONTROL_", "OPENSWMM_PERF")) or key in (
            "OPENSWMM_GW_CLOCK_CELLS", "DYLD_INSERT_LIBRARIES", "OMP_NUM_THREADS"
        ):
            env.pop(key)
    if profiled:
        env.update(OPENSWMM_PERF="1", OPENSWMM_GW_CLOCK_CELLS="1")
    waited, settled = settle(*quiet) if quiet else (0.0, None)
    load_before = os.getloadavg()[0]
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.perf_counter()
    with (dest / "console.log").open("w") as log:
        subprocess.run([str(cli), "model.inp", "model.rpt", "model.out"], cwd=dest, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    elapsed = time.perf_counter() - started
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime
    log = (dest / "console.log").read_text()
    assert "[PERF-PROCESS-INVALID]" not in log, dest
    assert b"ERROR " not in (dest / "model.rpt").read_bytes(), dest
    clocks = clock_rows(log)
    if profiled:
        validate_clock(clocks)
        if expected_seconds is not None:
            assert abs(float(clocks["summary"][0]["seconds"]) - expected_seconds) <= slack
        assert all(int(row["invalid_interval"]) == 0 for row in clocks["-CLOSURE"])
    else:
        assert not clocks
    phases = {group: dict(re.findall(r"([\w.]+)=([^\s]+)", body))
              for group, body in re.findall(r"\[PERF-([^]]+)\]([^\n]+)", log)}
    return {"wall_seconds": elapsed, "cpu_seconds": cpu,
            "load_average_1min": [load_before, os.getloadavg()[0]],
            "settle_wait_seconds": waited, "load_settled": settled, "out_sha256": sha(dest / "model.out"),
            "report_payload_sha256": hashlib.sha256(report_payload(dest / "model.rpt")).hexdigest(),
            "datasets": datasets(dest / "fields.2d.h5"), "clock": clocks, "phases": phases}


def phase_total(row, group):
    return sum(float(value) for key, value in row["phases"][group].items() if not key.startswith("n."))


def summarize(out, pairs):
    wall, cpu, load, gw, surface, firings, cost = {}, {}, {}, {}, {}, {}, {}
    for mode in ("SHARED", "INDEPENDENT"):
        wall[mode] = statistics.median(pair[mode]["plain"]["wall_seconds"] for pair in pairs)
        # Measurements saved before CPU time was recorded have neither key.
        cpu[mode] = statistics.median(pair[mode]["plain"].get("cpu_seconds", math.nan)
                                      for pair in pairs)
        load[mode] = max(max(pair[mode][label].get("load_average_1min", [math.nan]))
                         for pair in pairs for label in ("profiled", "plain"))
        gw[mode] = statistics.median(phase_total(pair[mode]["profiled"], "GW2D") for pair in pairs)
        surface[mode] = statistics.median(phase_total(pair[mode]["profiled"], "2D") for pair in pairs)
        counts = [int(pair[mode]["profiled"]["clock"]["summary"][0]["firings"]) for pair in pairs]
        assert len(set(counts)) == 1, (mode, counts)
        firings[mode] = counts[0]
        cost[mode] = gw[mode] / max(firings[mode], 1)
    fields = {}
    base = out / "0/SHARED/plain/fields.2d.h5"
    candidate = out / "0/INDEPENDENT/plain/fields.2d.h5"
    with h5py.File(base) as a, h5py.File(candidate) as b:
        np.testing.assert_array_equal(a["time"][:], b["time"][:])
        for name in ("Mesh2_face_gw_hg", "Mesh2_face_gw_dunne_cum", "Mesh2_face_depth"):
            x, y = a[name][:].astype(float), b[name][:].astype(float)
            delta = y - x
            norm = float(np.linalg.norm(x))
            fields[name] = {"max_abs": float(np.abs(delta).max()),
                            "relative_l2": float(np.linalg.norm(delta) / norm) if norm else None}
        budgets = {"SHARED": residuals(a), "INDEPENDENT": residuals(b)}
    ratio = gw["SHARED"] / gw["INDEPENDENT"]
    regression = surface["INDEPENDENT"] / surface["SHARED"] - 1
    return {"repeats": len(pairs), "median_unprofiled_wall_seconds": wall,
            "median_unprofiled_cpu_seconds": cpu,
            "unprofiled_cpu_speed_ratio": cpu["SHARED"] / cpu["INDEPENDENT"],
            "highest_load_average_1min": load,
            "median_gw_process_seconds": gw, "median_surface_process_seconds": surface,
            "physical_firings": firings, "gw_process_seconds_per_firing": cost,
            "gw_process_speed_ratio": ratio, "surface_process_relative_change": regression,
            "unprofiled_wall_speed_ratio": wall["SHARED"] / wall["INDEPENDENT"],
            "field_differences_vs_shared": fields, "budgets": budgets,
            "profile_on_off_exact": True,
            "observed_performance_targets": {"gw_process_10x": ratio >= 10,
                                             "surface_regression_at_most_2pct": regression <= .02}}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("engine", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--suite", choices=("soa", "bellinge"), required=True)
    parser.add_argument("--bellinge", type=Path)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--settle-load", type=float, metavar="MAX",
                        help="wait for the 1-minute load average to fall to MAX before each run")
    parser.add_argument("--settle-wait", type=float, default=900, metavar="SECONDS")
    parser.add_argument("--window", nargs=2, metavar=("START", "END"),
                        help='run only this part of the Bellinge event, "MM/DD/YYYY HH:MM:SS" each')
    args = parser.parse_args()
    window = None
    if args.window:
        if args.suite != "bellinge":
            parser.error("--window applies to the Bellinge suite")
        window = [datetime.datetime.strptime(value, "%m/%d/%Y %H:%M:%S") for value in args.window]
        if window[1] <= window[0]:
            parser.error("--window END must be after START")
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    if args.suite == "bellinge" and not args.bellinge:
        parser.error("--bellinge is required for the full event")
    sources = [FIXTURES / "soa_sigma.inp", FIXTURES / "soa_gw_species_heat.inp"]
    sources += sorted((FIXTURES / "pipeline_parallel").glob("mixed_water_t*.inp"))
    if args.suite == "bellinge":
        sources = [args.bellinge.resolve()]
    cli, out = args.engine.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    provenance = {"engine": engine(cli), "cli_sha256": sha(cli), "script_sha256": sha(Path(__file__)),
                  "suite": args.suite, "repeats": args.repeat, "window": args.window,
                  "edits": ["CPU backend", "local HDF5 output path", "explicit clock mode"],
                  "inputs": {str(p): sha(p) for p in sources},
                  "timing": "isolated sequential runs; unprofiled wall time; separate exclusive process timers"}
    prepared = {}
    for source in sources:
        for mode in ("SHARED", "INDEPENDENT"):
            prepared[source, mode] = prepare(source, mode, window)
    dependencies = {str(p.resolve()): sha(p) for _, deps in prepared.values() for p in deps}
    provenance["dependencies"] = dependencies
    path = out / "provenance.json"
    if path.exists():
        assert json.loads(path.read_text()) == provenance, "Inputs/build changed; use a new output directory"
    else:
        save(path, provenance)
    result = {"provenance": provenance, "status": "running", "cases": {}}
    try:
        for source in sources:
            name = source.stem
            if args.suite == "bellinge":
                name = "bellinge_window" if window else "bellinge_full"
            pairs = []
            for repeat in range(args.repeat):
                pair = {}
                modes = ("SHARED", "INDEPENDENT") if repeat % 2 == 0 else ("INDEPENDENT", "SHARED")
                for mode in modes:
                    pair[mode] = {}
                    text, deps = prepared[source, mode]
                    for label in ("profiled", "plain"):
                        dest = out / name / str(repeat) / mode / label
                        dest.mkdir(parents=True, exist_ok=True)
                        measurement = dest / "measurement.json"
                        save(out / "progress.json", {"status": "running", "case": name, "repeat": repeat,
                             "mode": mode, "measurement": label, "directory": str(dest)})
                        if measurement.exists():
                            pair[mode][label] = json.loads(measurement.read_text())
                            continue
                        (dest / "model.inp").write_text(text)
                        for dep in deps:
                            target = dest / dep.name
                            if not target.exists():
                                target.symlink_to(dep.resolve())
                        assert engine(cli) == provenance["engine"], "Engine changed during measurement"
                        print(f"Starting {name} #{repeat} {mode} {label}", flush=True)
                        expected = 172680 if args.suite == "bellinge" else None
                        if window:
                            expected = (window[1] - window[0]).total_seconds()
                        # A window that starts on a whole minute reports one second less
                        # than its length (7199 s for two hours); the full event is exact.
                        row = measure(cli, dest, label == "profiled", expected,
                                      1 + 1e-8 if window else 1e-8,
                                      (args.settle_load, args.settle_wait)
                                      if args.settle_load is not None else None)
                        pair[mode][label] = row
                        save(measurement, row)
                        print(f"Completed in {row['wall_seconds']:.3f} s", flush=True)
                    for key in ("out_sha256", "report_payload_sha256", "datasets"):
                        assert pair[mode]["plain"][key] == pair[mode]["profiled"][key], (dest, key)
                pairs.append(pair)
                summary = summarize(out / name, pairs)
                result["cases"][name] = {"summary": summary, "runs": pairs}
                save(out / "results.json", result)
                print(name, json.dumps({k: summary[k] for k in ("gw_process_speed_ratio",
                      "surface_process_relative_change", "unprofiled_wall_speed_ratio",
                      "unprofiled_cpu_speed_ratio")}), flush=True)
        result["status"] = "complete"
        save(out / "results.json", result)
        save(out / "progress.json", {"status": "complete"})
    except BaseException as error:
        result.update(status="failed", error=str(error))
        save(out / "results.json", result)
        raise


if __name__ == "__main__":
    main()
