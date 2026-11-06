#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Capture the native storage candidate, guarded adapter and FV regressions."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    "fv_baseline_capture", ROOT / "tests/verification/fv_implicit_phase0/run.py")
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)
TARGETS = {
    "test_engine_fv_nonlinear_pressure_storage": "test_fv_nonlinear_pressure_storage.cpp",
    "test_engine_fv_pressure_candidate_adapter": "test_fv_pressure_candidate_adapter.cpp",
    "test_engine_fv_pressure_reference": "test_fv_pressure_reference.cpp",
    "test_engine_fv_pressurized_implicit": "test_fv_pressurized_implicit.cpp",
    "test_engine_fv_network": "test_fv_solver_network.cpp",
    "test_engine_fv_tpa_closure": "test_fv_tpa_closure.cpp",
    "test_engine_fv_integration": "test_fv_engine_integration.cpp",
}
FILTERS = {"test_engine_fv_integration": ":".join((
    "FvEngine.FvOptionsSurviveAnInpRoundTrip",
    "FvEngine.FvKeysAreInertUnderOtherRoutingModels",
    "FvEngine.JunctionStorageIsCountedInTheRoutingBalance",
    "FvEngine.PondedNodeStillReportsItsFloodingRate"))}


def sources():
    result = baseline.sources()
    extra = [ROOT / "tests/unit/engine" / name for name in TARGETS.values()]
    extra += [Path(__file__).resolve()]
    extra += [ROOT / "tests/regression/data/Example1.inp"]
    for path in extra:
        result[str(path.relative_to(ROOT))] = baseline.digest(path)
    return dict(sorted(result.items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    build, out = args.build_dir.resolve(), args.output.resolve()
    if out.exists():
        parser.error("output must be new; prior evidence is never overwritten")
    out.mkdir(parents=True)
    report = {"status": "running", "source_before": sources(), "targets": {}}
    save = lambda: baseline.write_json(out / "manifest.json", report)
    save()
    try:
        report["head"] = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        env, removed = baseline.clean_environment()
        report["cleared_environment"] = removed
        report["run_environment"] = {k: v for k, v in env.items() if k.startswith(("OMP_", "KMP_"))}
        command = ["cmake", "--build", str(build), "--config", "Release",
                   "--parallel", str(args.jobs), "--target", *TARGETS]
        report["build_command"] = command
        with (out / "build.log").open("w") as log:
            subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=1800)
        if sources() != report["source_before"]:
            raise RuntimeError("source drift during build; recapture in a new directory")
        for name in ("CMakeCache.txt", "compile_commands.json"):
            shutil.copyfile(build / name, out / name)
        commands = json.loads((out / "compile_commands.json").read_text())
        relevant = [entry for entry in commands if entry["file"].endswith(
            ("/NonlinearPressureStorage.cpp", "/test_fv_nonlinear_pressure_storage.cpp"))]
        if len(relevant) != 2:
            raise RuntimeError("candidate/kernel compile invocations not uniquely identified")
        for entry in relevant:
            flags = entry.get("command", " ".join(entry.get("arguments", [])))
            if os.name != "nt" and ("-ffast-math" in flags or
                "-fno-fast-math" not in flags or "-ffp-contract=off" not in flags):
                raise RuntimeError("strict floating-point compilation required")
        report["candidate_compile_commands"] = relevant
        for name, expected in report["source_before"].items():
            dest = out / "source_snapshot" / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, dest)
            if baseline.digest(dest) != expected:
                raise RuntimeError(f"source drift while copying {name}")
        engine_file = build / "tests/verification/fv_implicit_phase0/engine_library_Release.txt"
        engine = Path(engine_file.read_text().strip()).resolve()
        artifacts = [engine] + [build / "bin/Release" / (t + (".exe" if os.name == "nt" else "")) for t in TARGETS]
        report["artifacts"] = {str(p): baseline.digest(p) for p in artifacts}
        (out / "artifact_snapshot").mkdir()
        for path in artifacts:
            dest = out / "artifact_snapshot" / path.name
            shutil.copyfile(path, dest)
            if baseline.digest(dest) != report["artifacts"][str(path)]:
                raise RuntimeError(f"artifact drift while copying {path}")
        # Each test writes only into its own persistent, reviewable directory.
        for target in TARGETS:
            work = out / target
            work.mkdir()
            binary = build / "bin/Release" / (target + (".exe" if os.name == "nt" else ""))
            with (work / "run.log").open("w") as log:
                command = [str(binary), f"--gtest_output=xml:{work / 'results.xml'}"]
                if target in FILTERS:
                    command += [f"--gtest_filter={FILTERS[target]}"]
                process = subprocess.run(
                    command,
                    cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=300)
            tree = ET.parse(work / "results.xml").getroot()
            report["targets"][target] = {
                "returncode": process.returncode, "tests": int(tree.attrib["tests"]),
                "failures": int(tree.attrib["failures"]), "disabled": int(tree.attrib["disabled"]),
                "filter": FILTERS.get(target, "*")}
            save()
        report["source_after"] = sources()
        report["later_working_tree_drift"] = [
            name for name in sorted(report["source_before"].keys() | report["source_after"].keys())
            if report["source_before"].get(name) != report["source_after"].get(name)]
        # Native test processes consume the frozen binaries, not this checkout's
        # sources. Reject build/copy drift above; retain later checkout drift
        # explicitly while verifying every tested artifact remains unchanged.
        report["qualification"] = "retained build-source snapshot and unchanged native artifacts"
        for name, expected in report["artifacts"].items():
            if baseline.digest(Path(name)) != expected:
                raise RuntimeError(f"runtime artifact changed: {name}")
        if any(v["returncode"] or v["failures"] for v in report["targets"].values()):
            raise RuntimeError("one or more native suites failed; see retained XML/logs")
        report["status"] = "passed"
        report["total_test_cases"] = sum(v["tests"] for v in report["targets"].values())
    except Exception as exc:
        report.update(status="failed", error=str(exc))
        raise
    finally:
        save()
    print(f"PASS: {report['total_test_cases']} native cases; evidence in {out}")


if __name__ == "__main__":
    main()
