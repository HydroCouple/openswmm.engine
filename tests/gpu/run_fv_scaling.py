"""Time the 1D finite volume solver on one deck: CPU loop against the device backend.

Runs, in this order, each in its own folder under --output/<deck name>/:

  cpu_global_t1   CPU solver, global stepping, one thread. The reference the device backend
                  must reproduce (same options, snapshot residual rule).
  cpu_global_tN   the same at --threads CPU threads.
  cpu_lts_tN      CPU solver with local time stepping on (the CPU default), --threads threads.
                  A different integration schedule, so its results differ from the others;
                  it is here as the practical CPU baseline for run time.
  device          the --backend plugin, with OPENSWMM_PERF=1.
  device_fenced   the same with OPENSWMM_PERF_FENCE=1 (per-phase device times); --fenced only.

All runs hold structure and boundary forcing over the routing step
(FV_STRUCTURE_COUPLING ROUTING_STEP), because that is what the device backend integrates.

Appends one row per run to --output/results.csv and prints a table. For the device runs it
reports whether the binary output and the end-of-run hot start are byte-identical to
cpu_global_t1, and otherwise the largest difference in the report's node and link summaries.
"""

import argparse
import csv
import filecmp
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

BASE = {"FV_COMPACTION": "NO", "FV_STRUCTURE_COUPLING": "ROUTING_STEP"}


def forced_deck(text, forced):
    keys = {k.upper() for k in forced}
    rows = ["%-22s %s" % item for item in forced.items()]
    out, in_options, written = [], False, False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("["):
            if in_options and not written:
                out += rows
                written = True
            in_options = stripped.upper().startswith("[OPTIONS]")
            out.append(line)
            continue
        if in_options and stripped and not stripped.startswith(";"):
            if stripped.split()[0].upper() in keys:
                continue
        out.append(line)
    if in_options and not written:
        out += rows
    files = [i for i, line in enumerate(out) if line.strip().upper().startswith("[FILES]")]
    if files:
        out.insert(files[0] + 1, "SAVE HOTSTART end_state.hsf")
    else:
        out += ["", "[FILES]", "SAVE HOTSTART end_state.hsf"]
    return "\n".join(out) + "\n"


def report_number(text, label):
    match = re.search(re.escape(label) + r"[ .:]*\s+(-?[\d.]+(?:[eE][-+]?\d+)?)", text)
    return float(match.group(1)) if match else float("nan")


def summary_rows(text, title):
    """Rows of one summary table of the report: name -> list of numeric fields."""
    rows = {}
    if title not in text:
        return rows
    body = text.split(title, 1)[1]
    body = body.split("\n  ***", 2)
    body = body[1] if len(body) > 1 else body[0]
    for line in body.splitlines():
        parts = line.split()
        if len(parts) < 4:
            continue
        numbers = []
        for token in parts[1:]:
            try:
                numbers.append(float(token))
            except ValueError:
                numbers.append(None)
        if sum(n is not None for n in numbers) >= 3:
            rows[parts[0]] = numbers
    return rows


def summary_difference(report_a, report_b):
    """Largest absolute difference, and the largest value, over the node and link summaries."""
    a, b = report_a.read_text(errors="replace"), report_b.read_text(errors="replace")
    worst, scale = {}, {}
    for title, column in (("Node Depth Summary", 2), ("Link Flow Summary", 1)):
        rows_a, rows_b = summary_rows(a, title), summary_rows(b, title)
        diff, big = 0.0, 0.0
        for name, values in rows_a.items():
            other = rows_b.get(name)
            if other is None or column >= len(values) or column >= len(other):
                continue
            if values[column] is None or other[column] is None:
                continue
            diff = max(diff, abs(values[column] - other[column]))
            big = max(big, abs(values[column]))
        worst[title], scale[title] = diff, big
    return worst, scale


def run_case(args, name, tag, text, env_extra, threads, side):
    folder = args.output / name / tag
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    for p in side:
        shutil.copy(p, folder)
    deck = folder / (name + ".inp")
    deck.write_text(text)
    env = dict(os.environ)
    for key in ("OPENSWMM_FV_BACKEND", "OPENSWMM_FV_SETTLE_SNAPSHOT", "OPENSWMM_PERF",
                "OPENSWMM_PERF_FENCE"):
        env.pop(key, None)
    env.update(env_extra)
    env["OMP_NUM_THREADS"] = str(threads)
    start = time.perf_counter()
    done = subprocess.run(
        [str(args.engine), str(deck), str(folder / "out.rpt"), str(folder / "out.out")],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=folder)
    wall = time.perf_counter() - start
    (folder / "console.txt").write_text(done.stdout)
    report = (folder / "out.rpt").read_text(errors="replace") if (folder / "out.rpt").exists() \
        else ""
    return {
        "deck": name, "run": tag, "threads": threads, "exit": done.returncode,
        "wall_s": round(wall, 3),
        "substeps": report_number(report, "Explicit Substeps"),
        "flux_evals": report_number(report, "Face Flux Evaluations"),
        "continuity_pct": report_number(report, "Continuity Error (%)"),
        "device_used": "[openswmm FV] using device backend" in done.stdout
                       and "not used" not in done.stdout,
        "folder": str(folder),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--plugin-dir", required=True, type=Path)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--backend", default="omp")
    parser.add_argument("--threads", default=8, type=int)
    parser.add_argument("--fenced", action="store_true", help="also run the fenced device case")
    parser.add_argument("--skip", action="append", default=[],
                        help="run tag to leave out (repeatable), e.g. cpu_lts_tN")
    args = parser.parse_args()
    # The engine runs inside each run folder, so every path must be absolute.
    args.engine = args.engine.resolve()
    args.plugin_dir = args.plugin_dir.resolve()
    args.input = args.input.resolve()
    args.output = args.output.resolve()

    name = args.input.stem
    source = args.input.read_text(errors="replace")
    side = [p for p in args.input.parent.iterdir()
            if p.is_file() and p.suffix.lower() not in (".inp", ".rpt", ".out", ".h5", ".hsf", ".py")
            and p.stat().st_size < 50e6]

    def deck(lts, threads):
        forced = dict(BASE)
        forced["FV_LTS"] = "YES" if lts else "NO"
        forced["THREADS"] = str(threads)
        return forced_deck(source, forced)

    cpu = {"OPENSWMM_FV_BACKEND": "cpu", "OPENSWMM_FV_SETTLE_SNAPSHOT": "1"}
    dev = {"OPENSWMM_FV_BACKEND": args.backend, "OPENSWMM_GPU_PLUGIN_PATH": str(args.plugin_dir),
           "OPENSWMM_PERF": "1"}
    n = args.threads
    cases = [("cpu_global_t1", deck(False, 1), cpu, 1),
             ("cpu_global_tN", deck(False, n), cpu, n),
             ("cpu_lts_tN", deck(True, n), {"OPENSWMM_FV_BACKEND": "cpu"}, n),
             ("device", deck(False, n), dev, n)]
    if args.fenced:
        cases.append(("device_fenced", deck(False, n), dict(dev, OPENSWMM_PERF_FENCE="1"), n))

    rows = []
    for tag, text, env, threads in cases:
        if tag in args.skip:
            continue
        row = run_case(args, name, tag, text, env, threads, side)
        rows.append(row)
        print("%-28s %-15s exit %d  %9.2f s  substeps %-10.0f continuity %s%%" % (
            name, tag, row["exit"], row["wall_s"], row["substeps"], row["continuity_pct"]),
            flush=True)

    reference = args.output / name / "cpu_global_t1"
    for row in rows:
        row["same_output"] = row["same_hotstart"] = ""
        row["max_node_depth_diff"] = row["max_link_flow_diff"] = ""
        if row["run"] == "cpu_global_t1" or not reference.exists():
            continue
        folder = Path(row["folder"])
        for key, item in (("same_output", "out.out"), ("same_hotstart", "end_state.hsf")):
            a, b = reference / item, folder / item
            row[key] = bool(a.exists() and b.exists() and filecmp.cmp(a, b, shallow=False))
        if (folder / "out.rpt").exists() and (reference / "out.rpt").exists():
            worst, scale = summary_difference(reference / "out.rpt", folder / "out.rpt")
            row["max_node_depth_diff"] = "%.4g of %.4g" % (
                worst["Node Depth Summary"], scale["Node Depth Summary"])
            row["max_link_flow_diff"] = "%.4g of %.4g" % (
                worst["Link Flow Summary"], scale["Link Flow Summary"])

    args.output.mkdir(parents=True, exist_ok=True)
    table = args.output / "results.csv"
    fields = ["deck", "run", "threads", "exit", "wall_s", "substeps", "flux_evals",
              "continuity_pct", "device_used", "same_output", "same_hotstart",
              "max_node_depth_diff", "max_link_flow_diff", "folder"]
    new = not table.exists()
    with table.open("a", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        if new:
            writer.writeheader()
        writer.writerows(rows)

    print()
    for row in rows:
        if row["run"].startswith("device"):
            print("%-15s device used: %s; output identical: %s; hot start identical: %s; "
                  "node depth diff %s; link flow diff %s" % (
                      row["run"], row["device_used"], row["same_output"], row["same_hotstart"],
                      row["max_node_depth_diff"], row["max_link_flow_diff"]))
    failed = [row for row in rows if row["exit"] != 0]
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
