"""Require the real OpenMP plugin and compare every 2D continuity report term."""

import argparse
import math
import os
from pathlib import Path
import re
import subprocess


def continuity(report):
    text = report.read_text(errors="replace")
    marker = "2D Surface Routing Continuity"
    if text.count(marker) != 1:
        raise AssertionError(f"{report}: expected one 2D continuity table")
    rows = {}
    for line in text.split(marker, 1)[1].splitlines():
        match = re.match(r"\s*(.*?)\s*\.{2,}\s+(\S+)", line)
        if not match:
            continue
        label, raw = match.groups()
        value = float(raw)
        if not math.isfinite(value) or label in rows:
            raise AssertionError(f"{report}: invalid row {line}")
        rows[label] = value
        if label == "Continuity Error (%)":
            break
    required = {
        "Initial Stored Volume", "Rainfall Inflow", "1D -> 2D Spill Inflow",
        "Outfall Inflow", "Boundary Inflow", "2D -> 1D Drain Outflow",
        "Outfall Withdrawal", "Boundary Outflow", "Evaporation Loss",
        "Infiltration Loss", "Final Stored Volume", "Continuity Error (%)",
    }
    if not required <= rows.keys():
        raise AssertionError(f"{report}: missing rows {required - rows.keys()}")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--plugin-dir", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--evaporation", action="store_true")
    args = parser.parse_args()
    deck = args.input.read_text()
    if args.evaporation:
        # 100 mm/hour makes applied evaporation visible at report precision,
        # including budget-limited dry-out in the shallow cells.
        deck, count = re.subn(r"(?m)^CONSTANT\s+0\.0\s*$", "CONSTANT 2400.0", deck)
        if count != 1:
            raise AssertionError("evaporation fixture did not replace exactly one rate")
        deck, count = re.subn(r"(?m)^\[2D_OPTIONS\]\s*$",
                              "[2D_OPTIONS]\nEVAPORATION CLIMATE", deck)
        if count != 1:
            raise AssertionError("evaporation fixture requires one 2D options section")
    results = {}
    for backend in ("cpu", "omp"):
        work = (args.output / backend).resolve()
        work.mkdir(parents=True, exist_ok=True)
        inp = work / "model.inp"
        report = work / "model.rpt"
        inp.write_text(deck)
        report.unlink(missing_ok=True)
        env = dict(os.environ, OPENSWMM_2D_BACKEND=backend,
                   OPENSWMM_GPU_PLUGIN_PATH=str(args.plugin_dir.resolve()),
                   OMP_NUM_THREADS="1", OMP_PROC_BIND="false", OPENSWMM_2D_HEAD_RAMP="0")
        run = subprocess.run([str(args.engine.resolve()), str(inp), str(report)],
                             cwd=work, env=env, text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=180)
        (work / "run.log").write_text(run.stdout)
        if run.returncode:
            raise AssertionError(f"{backend} exited {run.returncode}: {run.stdout[-4000:]}")
        if backend == "omp" and "using GPU backend: omp" not in run.stdout:
            raise AssertionError("OpenMP plugin was not selected; CPU fallback cannot pass")
        results[backend] = continuity(report)
    cpu, omp = results["cpu"], results["omp"]
    if cpu.keys() != omp.keys():
        raise AssertionError("CPU and plugin continuity rows differ")
    for label, reference in cpu.items():
        actual = omp[label]
        # Reports print three decimals. Allow one unit in the last printed
        # place plus a tight relative tolerance for large volumes.
        if not math.isclose(actual, reference, rel_tol=1e-7, abs_tol=0.0011):
            raise AssertionError(f"{label}: CPU={reference}, OpenMP={actual}")
        print(f"{label}: CPU={reference}, OpenMP={actual}")
    for backend, rows in results.items():
        if rows["Boundary Inflow"] <= 0 or rows["Boundary Outflow"] <= 0:
            raise AssertionError(f"{backend}: fixture must exercise both boundary directions")
        if abs(rows["Continuity Error (%)"]) > 0.001:
            raise AssertionError(f"{backend}: continuity ledger does not close")
        if args.evaporation and rows["Evaporation Loss"] <= 0:
            raise AssertionError(f"{backend}: evaporation was not exercised")


if __name__ == "__main__":
    main()
