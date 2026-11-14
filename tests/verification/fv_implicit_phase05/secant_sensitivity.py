#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Replay E2 with the existing secant-pass knob on the captured native build."""
import argparse
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("capture", ROOT / "tests/verification/fv_implicit_phase0/run.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    parent, out = args.capture.resolve(), args.output.resolve()
    if out.exists(): parser.error("output must be new")
    m = json.loads((parent / "manifest.json").read_text())
    assert m["status"] == "captured"
    out.mkdir(parents=True)
    binary = next(Path(p) for p in m["artifacts"] if Path(p).stem == "fv_pressure_qualification")
    for path, expected in m["artifacts"].items(): assert capture.digest(Path(path)) == expected, path
    source = parent / "e2_c50_p1/model.inp"
    expected_input = next(r["input_sha256"] for r in m["e2"] if r["run"] == "e2_c50_p1")
    assert capture.digest(source) == expected_input
    shutil.copyfile(Path(__file__), out / "secant_sensitivity.py")
    report = {"status":"running", "parent_manifest":str(parent / "manifest.json"),
        "parent_manifest_sha256":capture.digest(parent / "manifest.json"),
        "script_sha256":capture.digest(Path(__file__)), "artifacts":m["artifacts"],
        "input_sha256":expected_input, "rows":[]}
    save = lambda: capture.write_json(out / "manifest.json",report)
    save()
    env, removed = capture.clean_environment()
    report["cleared_environment"] = removed
    report["requested_workers"] = 1
    for passes in (1,2,3,4):
        work=out/f"passes_{passes}";work.mkdir()
        inp=work/"model.inp";shutil.copyfile(source,inp)
        child=dict(env,OPENSWMM_FV_PRESS_SECANT_PASSES=str(passes))
        with (work/"run.log").open("w") as log:
            p=subprocess.run([str(binary),"engine",str(inp),"30",str(work)],cwd=work,env=child,
                stdout=log,stderr=subprocess.STDOUT,timeout=300)
        result=json.loads((work/"summary.json").read_text())
        with (work/"engine_trace.csv").open() as stream: rows=list(csv.DictReader(stream))
        result.update(secant_passes=passes,returncode=p.returncode,
            peak_gauge_head_m=max(float(r["head_9p9_m"]) for r in rows),
            trace_sha256=capture.digest(work/"engine_trace.csv"))
        report["rows"].append(result);save()
    for path, expected in m["artifacts"].items(): assert capture.digest(Path(path)) == expected,path
    report["status"]="captured"
    report["limits"]="Gauge sampled at routing endpoints; not an internal-cell maximum or nonlinear residual history. No contraction claim."
    save()
    print(json.dumps(report["rows"],indent=2))


if __name__ == "__main__":main()
