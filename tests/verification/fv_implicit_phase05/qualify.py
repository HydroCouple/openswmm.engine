#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Hydraulic qualification evidence; failed physics gates remain failed."""
from __future__ import annotations

import argparse
import bisect
import csv
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("capture", ROOT / "tests/verification/fv_implicit_phase0/run.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)
DECK = ROOT / "docs/figures/decks/rapid_fill"
MODES = ("explicit", "legacy", "candidate")


def write(path, data):
    capture.write_json(path, data)


def sources():
    result = capture.sources()
    extra = [p for p in (ROOT / "tests/verification/fv_implicit_phase05").iterdir()
             if p.suffix in (".cpp", ".hpp", ".py")]
    extra += list(DECK.iterdir())
    extra += [ROOT / "tests/unit/engine/test_fv_unsteady_friction.cpp"]
    for p in extra:
        if p.is_file(): result[str(p.relative_to(ROOT))] = capture.digest(p)
    return dict(sorted(result.items()))


def matrix(profile):
    if profile == "smoke":
        return [(case, mode, 48, 150, cap, end)
                for case, cap, end in (("wave", .01, 2), ("front", .25, 120),
                                       ("drain", .25, 40), ("headloss", 1, 180)) for mode in MODES]
    rows = []
    for cells in (48, 96, 192, 384, 768):
        for c in (150, 660):
            for mode in MODES:
                rows.append(("wave", mode, cells, c, (300 / c) / (384 if cells == 768 else 192), 2))
    for steps in (96, 384):
        for c in (150, 660):
            for mode in MODES: rows.append(("wave", mode, 192, c, (300 / c) / steps, 2))
    for case, end in (("front", 120), ("drain", 40)):
        for cells in (48, 96, 192):
            for c in (150, 660):
                for mode in MODES: rows.append((case, mode, cells, c, .25, end))
        for c in (150, 660):
            for mode in MODES: rows.append((case, mode, 96, c, .125, end))
    for cells in (48, 96):
        for c in (150, 660):
            for mode in MODES: rows.append(("headloss", mode, cells, c, 1., 180))
    for dt in (1.e-3, 1.e-5, 1.e-7):
        for mode in MODES: rows.append(("momentum_limit", mode, 48, 150, dt, dt))
    return rows


def variant(text, replacements):
    lines, inside, seen = [], False, set()
    for line in text.splitlines():
        if line.strip().startswith("["):
            if inside:
                lines += [f"{k:24} {v}" for k, v in replacements.items() if k not in seen]
            inside = line.strip().upper() == "[OPTIONS]"
        tokens = line.split()
        if inside and tokens and tokens[0] in replacements:
            key = tokens[0]; line = f"{key:24} {replacements[key]}"; seen.add(key)
        lines.append(line)
    return "\n".join(lines) + "\n"


def interpolate(points, t):
    times = [p[0] for p in points]
    i = bisect.bisect_left(times, t)
    if i == 0: return points[0][1]
    if i == len(points): return points[-1][1]
    a, b = points[i-1], points[i]
    return a[1] + (b[1] - a[1]) * (t-a[0]) / (b[0]-a[0])


def pressure_observation_metrics(trace):
    with (DECK / "e2_pressure_9p9.csv").open() as f:
        observed = [(float(r["time_s"]), float(r["head_m"]))
                    for r in csv.DictReader(line for line in f if not line.startswith("#"))]
    with trace.open() as f:
        simulated = [(float(r["time_s"]), float(r["head_9p9_m"])) for r in csv.DictReader(f)]
    paired = [(h, interpolate(simulated, t)) for t, h in observed if simulated[0][0] <= t <= simulated[-1][0]]
    if not paired: return {"status": "no_overlap"}
    mean = sum(h for h, _ in paired) / len(paired)
    sse = sum((h-s)**2 for h,s in paired)
    variance = sum((h-mean)**2 for h,_ in paired)
    return {"status": "diagnostic_only", "pairs": len(paired), "time_offset_s": 0,
            "rmse_m": math.sqrt(sse/len(paired)), "nse": 1-sse/variance,
            "datum": "supplied header: +0.073 m already applied; no fitted offset"}


def analyze(rows):
    result = {"qualification": "not_promoted", "admissibility_failures": [],
              "wave_finest_gates": [], "headloss_gates": [], "temporal_pairs": [], "front_refinement": [],
              "zero_time_momentum_limit": []}
    for r in rows:
        if not r.get("completed") or r.get("max_relative_balance", 1) > 1.e-10 or not r.get("mesh_unchanged"):
            result["admissibility_failures"].append(r["run"])
        if r["case"] == "wave" and r["cells"] == 768 and r.get("completed"):
            ok = (r["wave_relative_rms"] <= .05 and abs(r["wave_peak_ratio"]-1) <= .05 and
                  r["wave_speed_relative_error"] <= .02)
            result["wave_finest_gates"].append({"run": r["run"], "passed": ok,
                "rms": r["wave_relative_rms"], "peak_ratio": r["wave_peak_ratio"],
                "speed_error": r["wave_speed_relative_error"]})
        if r["case"] == "headloss" and r.get("completed"):
            result["headloss_gates"].append({"run": r["run"],
                "passed": r["headloss_relative_q_error"] <= .001, "relative_q_error": r["headloss_relative_q_error"]})
        if r["case"] == "momentum_limit":
            result["zero_time_momentum_limit"].append({k:r.get(k) for k in (
                "mode","dt_cap_s","completed","interior_relative_q_change","filter_prediction_relative_error")})
    for c in (150,660):
        for mode in MODES:
            wave = sorted((r for r in rows if r["case"] == "wave" and r["cells"] == 192 and
                r["mode"] == mode and r["celerity_ft_s"] == c and r.get("completed")), key=lambda r:r["dt_cap_s"])
            result["temporal_pairs"].append({"mode":mode,"celerity_ft_s":c,
                "samples":[{k:r[k] for k in ("dt_cap_s","wave_relative_rms","wave_peak_ratio","wave_speed_relative_error")} for r in wave]})
            front = sorted((r for r in rows if r["case"] == "front" and r["mode"] == mode and
                r["celerity_ft_s"] == c and r["dt_cap_s"] == .25),key=lambda r:r["cells"])
            result["front_refinement"].append({"mode":mode,"celerity_ft_s":c,
                "samples":[{k:r.get(k) for k in ("cells","completed","arrival_s","rejections")} for r in front]})
    result["limits"] = ["E2 candidate unsupported: real storage tanks/weir; no simplified-boundary substitution.",
        "Digitized pressure is diagnostic; raster uncertainty and original extraction assets not recovered.",
        "Velocity/tank CSV headers cite a different paper; excluded from 2006-deck scoring.",
        "No independently converged moving/standing-front reference established by this matrix.",
        "Wave gates: RMS <=5%, peak error <=5%, speed error <=2% on chosen finest grid; not universal defaults.",
        "Headloss gate: Manning Q error <=0.1%; initial analytic seed is documented, not a startup convergence test."]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", choices=("smoke","full"), default="full")
    args = parser.parse_args()
    build, out = args.build_dir.resolve(), args.output.resolve()
    if out.exists(): parser.error("output must be new; failed evidence is retained")
    out.mkdir(parents=True)
    report = {"status":"running", "profile":args.profile, "source_before":sources(), "components":[]}
    save = lambda: write(out / "manifest.json", report)
    save()
    try:
        report["head"] = subprocess.check_output(["git","rev-parse","HEAD"], cwd=ROOT,text=True).strip()
        env, removed = capture.clean_environment()
        report["cleared_environment"] = removed
        report["run_environment"] = {k:v for k,v in env.items() if k.startswith(("OMP_","KMP_"))}
        command = ["cmake","--build",str(build),"--config","Release","--parallel","4","--target",
                   "fv_pressure_qualification","test_engine_fv_unsteady_friction"]
        with (out / "build.log").open("w") as log:
            subprocess.run(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=1800)
        if sources() != report["source_before"]: raise RuntimeError("source drift during build")
        for name in ("CMakeCache.txt","compile_commands.json"): shutil.copyfile(build/name,out/name)
        commands=json.loads((out/"compile_commands.json").read_text())
        checked=[r for r in commands if r["file"].endswith("/qualification.cpp") or "/hydraulics/fv/" in r["file"]]
        for row in checked:
            flags=row.get("command"," ".join(row.get("arguments",[])))
            if os.name != "nt" and ("-fno-fast-math" not in flags or "-ffp-contract=off" not in flags or "-ffast-math" in flags):
                raise RuntimeError("strict floating point required")
        for name,expected in report["source_before"].items():
            dest=out/"source_snapshot"/name;dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(ROOT/name,dest)
            if capture.digest(dest)!=expected: raise RuntimeError(f"copy drift: {name}")
        binary=build/"bin/Release"/("fv_pressure_qualification.exe" if os.name=="nt" else "fv_pressure_qualification")
        uf=build/"bin/Release"/("test_engine_fv_unsteady_friction.exe" if os.name=="nt" else "test_engine_fv_unsteady_friction")
        engine=Path((build/"tests/verification/fv_implicit_phase0/engine_library_Release.txt").read_text().strip()).resolve()
        report["artifacts"]={str(p):capture.digest(p) for p in (binary,uf,engine)}
        (out/"artifact_snapshot").mkdir()
        for p in (binary,uf,engine):shutil.copyfile(p,out/"artifact_snapshot"/p.name)
        for index,(case,mode,cells,c,cap,end) in enumerate(matrix(args.profile)):
            name=f"{index:03d}_{case}_{mode}_n{cells}_c{c}_dt{cap:.9g}"
            work=out/name;work.mkdir()
            started=time.monotonic()
            with (work/"run.log").open("w") as log:
                p=subprocess.run([str(binary),"component",case,mode,str(cells),str(c),str(cap),str(end),str(work)],
                    cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=180)
            summary=json.loads((work/"summary.json").read_text()) if (work/"summary.json").exists() else {
                "case":case,"mode":mode,"cells":cells,"celerity_ft_s":c,"dt_cap_s":cap,"completed":False}
            summary.update(run=name,returncode=p.returncode,wall_s=time.monotonic()-started,
                mesh_sha256=capture.digest(work/"mesh_before.csv"),
                mesh_unchanged=capture.digest(work/"mesh_before.csv")==capture.digest(work/"mesh_after.csv"),
                states_sha256=capture.digest(work/"states.csv"))
            report["components"].append(summary);save()
            print(name, "completed" if summary.get("completed") else "FAILED",flush=True)
        # Same authored mesh per case/celerity, independent of solver/time cap.
        fingerprints={}
        for row in report["components"]:
            key=(row["case"],row["cells"],row["celerity_ft_s"])
            if key in fingerprints and fingerprints[key]!=row["mesh_sha256"]: raise RuntimeError(f"mesh mismatch {key}")
            fingerprints[key]=row["mesh_sha256"]
        report["e2"] = []
        if args.profile=="full":
            text=(DECK/"rapid_fill.inp").read_text()
            for c in (25,50):
                for implicit in (False,True):
                    work=out/f"e2_c{c}_p{int(implicit)}";work.mkdir()
                    inp=work/"model.inp"
                    opts={"END_TIME":"00:00:30","FV_SLOT_CELERITY":str(c),"FV_LTS":"NO",
                          "FV_COMPACTION":"NO","FV_ORDER":"1","FV_PRESSURIZED_IMPLICIT":"YES" if implicit else "NO"}
                    inp.write_text(variant(text,opts))
                    with (work/"run.log").open("w") as log:
                        process=subprocess.run([str(binary),"engine",str(inp),"30",str(work)],cwd=work,env=env,
                            stdout=log,stderr=subprocess.STDOUT,timeout=300)
                    data=json.loads((work/"summary.json").read_text())
                    data.update(run=work.name,returncode=process.returncode,options=opts,input_sha256=capture.digest(inp),
                        pressure_observation=pressure_observation_metrics(work/"engine_trace.csv"))
                    report["e2"].append(data);save();print(work.name,data["completed"],flush=True)
            work=out/"uf_implicit_regression";work.mkdir()
            with (work/"run.log").open("w") as log:
                p=subprocess.run([str(uf),"--gtest_filter=FvUnsteadyFriction.ValveClosureDampsOnImplicitPath",
                    f"--gtest_output=xml:{work/'results.xml'}"],cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
            xml=ET.parse(work/"results.xml").getroot()
            report["uf_regression"]={"returncode":p.returncode,"tests":int(xml.attrib["tests"]),"failures":int(xml.attrib["failures"])}
        report["source_after"]=sources()
        report["later_working_tree_drift"]=[k for k in sorted(report["source_before"].keys()|report["source_after"].keys())
            if report["source_before"].get(k)!=report["source_after"].get(k)]
        for name,expected in report["artifacts"].items():
            if capture.digest(Path(name))!=expected: raise RuntimeError(f"runtime artifact changed: {name}")
        write(out/"hydraulic_assessment.json",analyze(report["components"]))
        report["status"]="captured"  # hydraulic acceptance is separate and remains explicit
        report["qualification"]="not_promoted"
    except Exception as exc:
        report.update(status="capture_failed",error=str(exc));raise
    finally:save()
    print(f"Captured {len(report['components'])} component cases; hydraulic gates in hydraulic_assessment.json")


if __name__=="__main__":main()
