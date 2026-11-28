#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Capture the native coupled reach residual and source-matched regressions."""
import argparse
import csv
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


base = module("baseline", ROOT / "tests/verification/fv_implicit_phase0/run.py")
previous = module("adapter", ROOT / "tests/verification/fv_implicit_phase05/run.py")
TARGETS = dict(previous.TARGETS, test_engine_fv_implicit_reach="test_fv_implicit_reach.cpp",
               test_engine_native_gmres="test_native_gmres.cpp")


def sources():
    result = previous.sources()
    extra = list((ROOT / "tests/verification/fv_implicit_phase1").glob("*"))
    extra += [ROOT / "tests/unit/engine/test_fv_implicit_reach.cpp",ROOT / "tests/unit/engine/test_native_gmres.cpp"]
    for path in extra:
        if path.suffix in (".cpp", ".hpp", ".py"):
            result[str(path.relative_to(ROOT))] = base.digest(path)
    return dict(sorted(result.items()))


def matrix(smoke):
    if smoke:
        for mode in ("explicit", "coupled"):
            yield mode, "wave", 96, 150, .5 * 1200 / 96 / 150, 1
            yield mode, "momentum", 48, 150, 1.e-7, 1.e-7
        yield "coupled", "headloss", 48, 150, .25, 180
        return
    for cells in (96, 192, 384, 768):
        for c in (150, 660):
            for mode in ("explicit", "coupled"):
                yield mode, "wave", cells, c, .5 * 1200 / cells / c, 1
    for c in (150, 660):
        for cfl in (.25, .125, .0625):
            for mode in ("explicit", "coupled"):
                yield mode, "wave", 192, c, cfl * 1200 / 192 / c, 1
        for cells, cfl in ((1536, .125), (3072, .25), (6144, .25)):
            yield "coupled", "wave", cells, c, cfl * 1200 / cells / c, 1
    for cfl in (2, 10, 100):
        yield "coupled", "wave", 96, 660, cfl * 1200 / 96 / 660, 1
    for c in (150, 660):
        for dt in (1.e-3, 1.e-5, 1.e-7):
            for mode in ("explicit", "coupled"):
                yield mode, "momentum", 48, c, dt, dt
        for cells in (48, 96, 192):
            for mode in ("explicit", "coupled"):
                yield mode, "headloss", cells, c, .25, 180


def audit(work, row):
    assert base.digest(work / "mesh_before.csv") == base.digest(work / "mesh_after.csv")
    with (work / "states.csv").open() as stream:
        states = list(csv.DictReader(stream))
    initial, final = states[:row["cells"]], states[row["cells"]:]
    assert len(final) == len(initial) == row["cells"]
    assert all(float(r["head_ft"]) >= 3.01 and all(math.isfinite(float(v)) for v in r.values()) for r in states)
    independent = {}
    if row["case"] == "wave":
        with (work / "mesh_before.csv").open() as stream:
            section = next(r for r in csv.reader(stream) if r[0] == "section")
        width = float(section[4])
        # Baseline area is independently reconstructed from the initial
        # Gaussian head and the linear slot relation, rather than the peak cell.
        speed = math.sqrt(32.2 * (float(initial[0]["area_ft2"]) - width * (float(initial[0]["head_ft"]) - 6)) / width)
        assert abs(speed - row["effective_speed_ft_s"]) < 1.e-10 * speed
        predicted = [.001 * math.exp(-.5 * ((float(r["x_ft"]) - 300 - speed * row["reached_s"]) / 60)**2) for r in final]
        rms = math.sqrt(sum((float(r["head_ft"]) - 6 - p)**2 for r, p in zip(final, predicted)) / sum(p*p for p in predicted))
        peak = max(float(r["head_ft"]) - 6 for r in final) / .001
        assert abs(rms - row["wave_relative_rms"]) < 1.e-10
        assert abs(peak - row["peak_ratio"]) < 1.e-10
        independent.update(wave_relative_rms=rms, peak_ratio=peak)
    if row["case"] == "momentum":
        a, b = row["cells"] // 4, 3 * row["cells"] // 4
        change = math.sqrt(sum((float(final[i]["discharge_cfs"]) - float(initial[i]["discharge_cfs"]))**2 for i in range(a,b)) /
                           sum(float(initial[i]["discharge_cfs"])**2 for i in range(a,b)))
        assert abs(change - row["relative_q_change"]) < 1.e-12
        independent["relative_q_change"] = change
    return independent


def gmres_matrix():
    for cells in (96,192):
        for c in (150,660):
            for mode,restart in (("coupled",40),("gmres",20),("gmres",40),("gmres",80)):
                yield mode,"wave",cells,c,.5*1200/cells/c,1,restart
    for c in (150,660):
        for dt in (1.e-3,1.e-5,1.e-7):
            for mode,restart in (("coupled",40),("gmres",20),("gmres",40),("gmres",80)):
                yield mode,"momentum",48,c,dt,dt,restart
        for cells in (48,96):
            for mode in ("coupled","gmres"):
                yield mode,"headloss",cells,c,.25,180,40
    for cfl in (2,10):
        for mode in ("coupled","gmres"):
            yield mode,"wave",96,660,cfl*1200/96/660,1,40


def compare_linear_modes(out, rows):
    direct={(r["case"],r["cells"],r["celerity_ft_s"],r["dt_cap_s"]):r for r in rows if r["mode"]=="coupled"}
    comparisons=[]
    for row in rows:
        if row["mode"]!="gmres": continue
        reference=direct[(row["case"],row["cells"],row["celerity_ft_s"],row["dt_cap_s"])]
        def final(r):
            with (out/r["run"]/"states.csv").open() as stream: return list(csv.DictReader(stream))[r["cells"]:]
        a,b=final(reference),final(row)
        errors={field:max(abs(float(x[field])-float(y[field])) for x,y in zip(a,b))
                for field in ("head_ft","area_ft2","discharge_cfs")}
        assert errors["head_ft"]<=1.e-8 and errors["area_ft2"]<=1.e-10 and errors["discharge_cfs"]<=1.e-8,errors
        comparisons.append({"run":row["run"],"reference":reference["run"],"max_absolute_differences":errors})
    return comparisons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    profile=parser.add_mutually_exclusive_group()
    profile.add_argument("--smoke", action="store_true")
    profile.add_argument("--gmres", action="store_true", help="direct/GMRES hydraulic comparison matrix")
    args = parser.parse_args()
    build, out = args.build_dir.resolve(), args.output.resolve()
    if out.exists(): parser.error("output must be new")
    out.mkdir(parents=True)
    m = {"status":"running", "source_before":sources(), "tests":{}, "components":[],
         "scope":"Standalone fully pressurized flat prismatic reach; no production solver selection", "requested_workers":1,
         "hydraulic_profile":"gmres_comparator" if args.gmres else "coupled_reach"}
    save = lambda: base.write_json(out / "manifest.json", m)
    save()
    try:
        env, removed = base.clean_environment(); m["cleared_environment"] = removed
        m["head"] = subprocess.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip()
        command = ["cmake","--build",str(build),"--config","Release","--parallel","4","--target","fv_implicit_reach",*TARGETS]
        m["build_command"] = command
        with (out / "build.log").open("w") as log:
            subprocess.run(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=1800)
        assert sources() == m["source_before"], "source drift during build"
        for name in ("CMakeCache.txt","compile_commands.json"): shutil.copyfile(build/name,out/name)
        commands = json.loads((out / "compile_commands.json").read_text())
        selected = [r for r in commands if r["file"].endswith(("/ImplicitReachSolver.cpp","/NativeGmres.cpp","/test_native_gmres.cpp","/test_fv_implicit_reach.cpp","/fv_implicit_phase1/driver.cpp"))]
        assert len(selected) == 5
        for r in selected:
            flags = r.get("command"," ".join(r.get("arguments",[])))
            assert os.name == "nt" or ("-ffast-math" not in flags and "-fno-fast-math" in flags and "-ffp-contract=off" in flags)
        m["compile_commands"] = selected
        for name, expected in m["source_before"].items():
            dest = out / "source_snapshot" / name; dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(ROOT/name,dest); assert base.digest(dest) == expected, name
        engine = Path((build / "tests/verification/fv_implicit_phase0/engine_library_Release.txt").read_text().strip()).resolve()
        binary = lambda name: build / "bin/Release" / (name + (".exe" if os.name == "nt" else ""))
        artifacts = [engine,binary("fv_implicit_reach"),*(binary(n) for n in TARGETS)]
        m["artifacts"] = {str(p):base.digest(p) for p in artifacts}
        (out / "artifact_snapshot").mkdir()
        for p in artifacts:
            dest=out/"artifact_snapshot"/p.name; shutil.copyfile(p,dest); assert base.digest(dest)==m["artifacts"][str(p)]
        for name in TARGETS:
            work=out/name; work.mkdir()
            command=[str(binary(name)),f"--gtest_output=xml:{work/'results.xml'}"]
            if name in previous.FILTERS: command += [f"--gtest_filter={previous.FILTERS[name]}"]
            with (work / "run.log").open("w") as log:
                p=subprocess.run(command,cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
            xml=ET.parse(work/"results.xml").getroot()
            m["tests"][name]={"returncode":p.returncode,"tests":int(xml.attrib["tests"]),"failures":int(xml.attrib["failures"]),"filter":previous.FILTERS.get(name,"*")}; save()
        mesh_groups={}
        cases=gmres_matrix() if args.gmres else ((*v,40) for v in matrix(args.smoke))
        for i, values in enumerate(cases):
            mode,case,cells,c,cap,end,restart=values
            work=out/f"case_{i:03d}_{case}_{mode}"; work.mkdir()
            command=[str(binary("fv_implicit_reach")),mode,case,str(cells),str(c),repr(cap),str(end),str(work),str(restart)]
            with (work / "run.log").open("w") as log:
                process=subprocess.run(command,cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
            row=json.loads((work/"summary.json").read_text())
            row.update(run=work.name,returncode=process.returncode,mesh_sha256=base.digest(work/"mesh_before.csv"),states_sha256=base.digest(work/"states.csv"),trace_sha256=base.digest(work/"trace.csv"))
            m["components"].append(row); save()
            assert process.returncode == 0 and row["completed"], row
            row["independent_metrics"]=audit(work,row)
            assert row["max_relative_balance"] <= 1.e-10
            assert mode == "explicit" or row["max_scaled_residual"] <= 1.0
            key=(case,cells,c)
            if key in mesh_groups: assert mesh_groups[key] == row["mesh_sha256"]
            mesh_groups[key]=row["mesh_sha256"]
            save()
        if args.gmres: m["linear_mode_comparisons"]=compare_linear_modes(out,m["components"])
        if args.gmres:
            m["jacobi_diagnostics"]=[]
            diagnostics=(("headloss",96,660,.25,180),("wave",96,660,10*1200/96/660,1),
                         ("wave",96,660,.5*1200/96/660,1))
            for i,(case,cells,c,cap,end) in enumerate(diagnostics):
                work=out/f"jacobi_diagnostic_{i:02d}"; work.mkdir()
                command=[str(binary("fv_implicit_reach")),"jacobi",case,str(cells),str(c),repr(cap),str(end),str(work),"40"]
                with (work/"run.log").open("w") as log:
                    process=subprocess.run(command,cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
                row=json.loads((work/"summary.json").read_text())
                assert process.returncode in (0,2) and row["completed"]==(process.returncode==0)
                row.update(run=work.name,returncode=process.returncode,mesh_sha256=base.digest(work/"mesh_before.csv"),
                           states_sha256=base.digest(work/"states.csv"),trace_sha256=base.digest(work/"trace.csv"))
                row["independent_metrics"]=audit(work,row)
                if not row["completed"] and row["reached_s"]==0:
                    with (work/"states.csv").open() as stream: states=list(csv.DictReader(stream))
                    assert all(all(a[field]==b[field] for field in a) for a,b in zip(states[:cells],states[cells:]))
                m["jacobi_diagnostics"].append(row);save()
        assert not any(r["returncode"] or r["failures"] for r in m["tests"].values())
        m["source_after"]=sources()
        m["later_working_tree_drift"]=[n for n in sorted(m["source_after"].keys()|m["source_before"].keys()) if m["source_after"].get(n)!=m["source_before"].get(n)]
        for p,expected in m["artifacts"].items(): assert base.digest(Path(p))==expected,p
        m["status"]="captured"
        m["qualification"]=("native GMRES/reach fallback increment; production promotion remains on hold" if args.gmres else
                            "bounded P1 residual increment; production promotion remains on hold")
        m["test_cases"]=sum(r["tests"] for r in m["tests"].values())
    except Exception as exc:
        m.update(status="failed",error=str(exc)); raise
    finally: save()
    print(f"Captured {len(m['components'])} hydraulic runs and {m['test_cases']} passing native test cases.")


if __name__ == "__main__": main()
