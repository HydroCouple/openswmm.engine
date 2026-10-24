#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Capture bounded physical storage; broad qualification remains deferred."""
import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET
import workspace_run as previous
import storage_audit as audit

ROOT=previous.ROOT
base=previous.base
TARGETS=dict(previous.TARGETS,test_engine_fv_physical_storage="test_fv_physical_storage.cpp")


def sources():
    result=previous.sources()
    p=ROOT/"tests/unit/engine/test_fv_physical_storage.cpp"
    result[str(p.relative_to(ROOT))]=base.digest(p)
    return dict(sorted(result.items()))


def comparisons(out,rows):
    results=previous.previous.compare(out,rows)
    for result in results:
        run=out/result["run"]; ref=out/result["reference"]
        # Include every retained node head and physical volume, beyond the
        # existing original-cell comparisons, at every accepted timestep.
        for file,key,limit in (("nodes.csv","head_ft",1.e-8),("storage.csv","volume_ft3",1.e-8)):
            read=lambda p:list(csv.DictReader((p/file).open()))
            a,b=read(run),read(ref); assert len(a)==len(b)
            error=max(abs(float(x[key])-float(y[key])) for x,y in zip(a,b))
            assert error<=limit,(run,file,error)
            result["errors"][key]=error
    return results


def review(out):
    m=json.loads((out/"manifest.json").read_text()); assert m["status"]=="captured"
    for name,expected in m["source_before"].items(): assert base.digest(out/"source_snapshot"/name)==expected,name
    for path,expected in m["artifacts"].items(): assert base.digest(out/"artifact_snapshot"/Path(path).name)==expected,path
    for name in ("storage_run.py","storage_audit.py","workspace_run.py","block_ilu_run.py","tree_run.py","condensation_run.py","network_run.py","network_review.py","run.py"):
        p=Path(__file__).resolve().parent/name
        assert base.digest(p)==m["source_before"][str(p.relative_to(ROOT))],p
    for target,row in m["tests"].items():
        xml=ET.parse(out/target/"results.xml").getroot()
        assert int(xml.attrib["tests"])==row["tests"] and int(xml.attrib["failures"])==int(xml.attrib["errors"])==0
    for row in m["networks"]:
        work=out/row["run"]
        for name,expected in row["files"].items(): assert base.digest(work/name)==expected,name
        if row["case"].startswith("storage_"):
            assert audit.audit(work,row)==row["independent"]
            assert audit.linear(work,row)==row["linear_audit"]
        else:
            assert previous.previous.network.audit(work,row)==row["independent"]
            assert previous.audit_linear(work,row)==row["linear_audit"]
        assert previous.previous.physics.face_physics(work,row)==row["face_physics"]
    for row in m["prior_controls"]+m["reuse_controls"]:
        for name in row["files"]: assert base.digest(out/row["run"]/name)==base.digest(Path(row["reference"])/name),name
    assert comparisons(out,[r for r in m["networks"] if r["case"].startswith("storage_")])==m["comparisons"]
    base.write_json(out/"review.json",{"manifest_sha256":base.digest(out/"manifest.json"),
        "native_cases":m["test_cases"],"storage_runs":sum(r["case"].startswith("storage_") for r in m["networks"]),
        "unchanged_prior_controls":len(m["prior_controls"]),"bit_identical_reuse_controls":len(m["reuse_controls"]),
        "method_comparisons":len(m["comparisons"]),"production_promotion":False,
        "scope":"focused physical storage implementation checks; broad hydraulic/performance qualification deferred"})
    print(f"Reviewed {m['test_cases']} native cases, {len(m['networks'])} short runs.")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir",type=Path); parser.add_argument("--output",type=Path); parser.add_argument("--review",type=Path)
    args=parser.parse_args()
    if args.review: review(args.review.resolve()); return
    if not args.build_dir or not args.output: parser.error("--build-dir and --output required")
    out=args.output.resolve(); build=args.build_dir.resolve()
    if out.exists(): parser.error("output must be new")
    out.mkdir(parents=True)
    m={"status":"running","source_before":sources(),"tests":{},"networks":[],"prior_controls":[],"reuse_controls":[],
       "production_promotion":False,"performance_campaign":"deferred by user"}
    save=lambda:base.write_json(out/"manifest.json",m)
    save()
    try:
        env,removed=base.clean_environment(); m["cleared_environment"]=removed
        m["head"]=subprocess.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip()
        command=["cmake","--build",str(build),"--config","Release","--parallel","4","--target","fv_implicit_network",*TARGETS]
        m["build_command"]=command
        with (out/"build.log").open("w") as log:
            subprocess.run(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=1800)
        assert sources()==m["source_before"],"source drift during build"
        for name in ("CMakeCache.txt","compile_commands.json"): shutil.copyfile(build/name,out/name)
        commands=json.loads((out/"compile_commands.json").read_text())
        names=("/ImplicitNetworkSolver.cpp","/NativeConduitCondensation.cpp","/NativeBlockIlu.cpp","/NativeLinearWorkspace.cpp","/NativeGmres.cpp","/network_driver.cpp",*("/"+v for v in TARGETS.values()))
        selected=[r for r in commands if r["file"].endswith(names)]; assert len(selected)==13
        for r in selected:
            flags=r.get("command"," ".join(r.get("arguments",[])))
            assert os.name=="nt" or ("-ffast-math" not in flags and "-fno-fast-math" in flags and "-ffp-contract=off" in flags)
        m["compile_commands"]=selected
        for name,expected in m["source_before"].items():
            dest=out/"source_snapshot"/name; dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(ROOT/name,dest); assert base.digest(dest)==expected,name
        assert sources()==m["source_before"],"source drift during archive"
        binary=lambda name:build/"bin/Release"/(name+(".exe" if os.name=="nt" else ""))
        engine=Path((build/"tests/verification/fv_implicit_phase0/engine_library_Release.txt").read_text().strip()).resolve()
        artifacts=[engine,binary("fv_implicit_network"),*(binary(name) for name in TARGETS)]
        m["artifacts"]={str(p):base.digest(p) for p in artifacts}; (out/"artifact_snapshot").mkdir()
        for p in artifacts:
            dest=out/"artifact_snapshot"/p.name; shutil.copyfile(p,dest); assert base.digest(dest)==m["artifacts"][str(p)]
        for target in TARGETS:
            work=out/target; work.mkdir()
            with (work/"run.log").open("w") as log:
                process=subprocess.run([str(binary(target)),f"--gtest_output=xml:{work/'results.xml'}"],cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
            xml=ET.parse(work/"results.xml").getroot()
            m["tests"][target]={"returncode":process.returncode,"tests":int(xml.attrib["tests"]),"failures":int(xml.attrib["failures"])}; save()
            assert process.returncode==0 and int(xml.attrib["failures"])==int(xml.attrib["errors"])==0,target
        modes=("full","condensed","reduced","tree_reduced","full_block","reduced_block","tree_reduced_block","fallback_block",
               "full_block_reuse","reduced_block_reuse","tree_reduced_block_reuse")
        cases=[(name,mode,True) for mode in modes for name in ("storage_tee","storage_loop","storage_reversed","storage_closed")]
        cases.extend((name,"full",False) for name in ("storage_tee","storage_loop","storage_reversed","storage_closed"))
        reference=ROOT/"tests/output/fv_implicit_phase1/2026-10-20_workspace_final"
        prior=json.loads((reference/"manifest.json").read_text())
        controls=[r for r in prior["networks"] if (r["case"] in ("tee_source","loop") and not r["mode"].endswith("_reuse")) or
                  (r["case"] in ("bed","geometry","virtual","reversed") and r["mode"]=="full")]
        cases.extend((r["case"],r["mode"],True) for r in controls)
        for name,mode,moving in cases:
            work=out/f"case_{len(m['networks']):03d}_{name}_{mode}"; work.mkdir()
            command=[str(binary("fv_implicit_network")),name,"16","660",".25","1",str(work),"40",str(int(moving)),mode]
            with (work/"run.log").open("w") as log:
                process=subprocess.run(command,cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
            row=json.loads((work/"summary.json").read_text()); row.update(run=work.name,returncode=process.returncode)
            m["networks"].append(row); save()
            assert process.returncode==0 and row["completed"],row
            row["independent"]=audit.audit(work,row) if name.startswith("storage_") else previous.previous.network.audit(work,row)
            row["face_physics"]=previous.previous.physics.face_physics(work,row)
            row["linear_audit"]=audit.linear(work,row) if name.startswith("storage_") else previous.audit_linear(work,row)
            row["files"]={p.name:base.digest(p) for p in work.iterdir() if p.suffix in (".csv",".closures")}
            if not name.startswith("storage_"):
                ref=reference/next(r["run"] for r in controls if r["case"]==name and r["mode"]==mode)
                files=("states.csv","nodes.csv","faces.csv","trace.csv","mesh_before.csv","mesh_after.csv","mesh_before.csv.closures","mesh_after.csv.closures")
                for file in files: assert base.digest(work/file)==base.digest(ref/file),(work,file)
                m["prior_controls"].append({"run":work.name,"reference":str(ref),"files":files})
            save()
        for row in m["networks"]:
            if not row["mode"].endswith("_reuse"): continue
            ref=out/next(r["run"] for r in m["networks"] if r["case"]==row["case"] and r["mode"]==row["mode"][:-6])
            files=tuple(name for name in row["files"] if name!="linear.csv")
            for file in files: assert base.digest(out/row["run"]/file)==base.digest(ref/file),(row["run"],file)
            m["reuse_controls"].append({"run":row["run"],"reference":str(ref),"files":files})
        m["comparisons"]=comparisons(out,[r for r in m["networks"] if r["case"].startswith("storage_")])
        m["source_after"]=sources()
        m["later_working_tree_drift"]=[name for name in sorted(m["source_before"].keys()|m["source_after"].keys()) if m["source_before"].get(name)!=m["source_after"].get(name)]
        for path,expected in m["artifacts"].items(): assert base.digest(Path(path))==expected,path
        m["test_cases"]=sum(r["tests"] for r in m["tests"].values()); m["status"]="captured"
    except Exception as exc:
        m.update(status="failed",error=str(exc)); raise
    finally: save()
    review(out)


if __name__=="__main__": main()
