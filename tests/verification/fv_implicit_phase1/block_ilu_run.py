#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Focused native block-ILU checks; broad hydraulic/performance campaign deferred."""
import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET
import tree_run as tree
import condensation_run as previous

ROOT=previous.ROOT
base=previous.base
TARGETS=dict(tree.TARGETS,test_engine_fv_block_ilu="test_fv_block_ilu.cpp")


def sources():
    result=tree.sources()
    p=ROOT/"tests/unit/engine/test_fv_block_ilu.cpp"
    result[str(p.relative_to(ROOT))]=base.digest(p)
    return dict(sorted(result.items()))


def audit_linear(work,row):
    mode=row["mode"]
    if not mode.endswith("_block"): return tree.audit_linear(work,row)
    method=mode[:-6]
    with (work/"linear.csv").open() as stream: lines=list(csv.DictReader(stream))
    assert len(lines)==row["newton"] and lines
    total=0
    for r in lines:
        assert int(r["accepted_step"])==int(r["linear_converged"])==1
        assert float(r["full_true_residual"])<=float(r["full_tolerance"])
        assert int(r["buffer_bound_bytes"])<=64*1024*1024
        total+=int(r["gmres_iterations"])+(int(r["reduced_iterations"]) if int(r["fallback"]) else 0)
        if method=="full":
            assert int(r["condensation_status"])==0 and int(r["fallback"])==0 and int(r["block_ilu"])==1
        elif method=="fallback":
            assert int(r["condensation_status"])==6 and int(r["fallback"])==int(r["block_ilu"])==1
        else:
            assert int(r["condensation_status"])==1 and int(r["fallback"])==0 and int(r["dense"])==0
            if method=="tree_reduced":
                assert int(r["tree"])==1
                assert int(r["tree_eliminated"])+int(r["core_scalars"])==int(r["retained"])
            expected=method=="reduced" or row["case"]=="loop"
            assert int(r["block_ilu"])==int(r["reduced_block_ilu"])==int(r["reduced_gmres"])==expected
        if int(r["block_ilu"]):
            assert int(r["ilu_blocks"])>0 and int(r["ilu_entries"])>=int(r["ilu_blocks"])
            assert int(r["ilu_failed_block"])==-1
    assert total==row["gmres"]
    return {"corrections":len(lines),
            "max_residual_ratio":max(float(r["full_true_residual"])/float(r["full_tolerance"]) for r in lines),
            "block_ilu_attempts":sum(int(r["block_ilu"]) for r in lines),
            "shifted_blocks":sum(int(r["shifted_pivots"]) for r in lines)}


def review(out):
    m=json.loads((out/"manifest.json").read_text())
    assert m["status"]=="captured" and m["production_promotion"] is False
    for name,expected in m["source_before"].items():
        assert base.digest(out/"source_snapshot"/name)==expected,name
    for path,expected in m["artifacts"].items():
        assert base.digest(out/"artifact_snapshot"/Path(path).name)==expected,path
    for name in ("block_ilu_run.py","tree_run.py","condensation_run.py","network_run.py","network_review.py","run.py"):
        p=Path(__file__).resolve().parent/name
        assert base.digest(p)==m["source_before"][str(p.relative_to(ROOT))],p
    for target,row in m["tests"].items():
        xml=ET.parse(out/target/"results.xml").getroot()
        assert int(xml.attrib["tests"])==row["tests"] and int(xml.attrib["failures"])==int(xml.attrib["errors"])==0
    for row in m["networks"]:
        work=out/row["run"]
        for name,expected in row["files"].items(): assert base.digest(work/name)==expected,name
        assert previous.network.audit(work,row)==row["independent"]
        assert previous.physics.face_physics(work,row)==row["face_physics"]
        assert audit_linear(work,row)==row["linear_audit"]
    assert previous.compare(out,m["networks"])==m["comparisons"]
    for row in m["prior_controls"]:
        for name in row["files"]:
            assert base.digest(out/row["run"]/name)==base.digest(Path(row["reference"])/name),name
    base.write_json(out/"review.json",{"manifest_sha256":base.digest(out/"manifest.json"),
        "audit_sha256":base.digest(Path(__file__)),"native_cases":m["test_cases"],
        "network_runs":len(m["networks"]),"method_comparisons":len(m["comparisons"]),
        "unchanged_prior_controls":len(m["prior_controls"]),"production_promotion":False,
        "scope":"focused implementation checks; not the deferred broad hydraulic/performance qualification"})
    print(f"Reviewed {m['test_cases']} native tests, {len(m['networks'])} focused network runs.")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir",type=Path)
    parser.add_argument("--output",type=Path)
    parser.add_argument("--review",type=Path)
    args=parser.parse_args()
    if args.review:
        review(args.review.resolve()); return
    if not args.build_dir or not args.output: parser.error("--build-dir and --output are required")
    build=args.build_dir.resolve(); out=args.output.resolve()
    if out.exists(): parser.error("output must be new")
    out.mkdir(parents=True)
    m={"status":"running","source_before":sources(),"tests":{},"networks":[],"prior_controls":[],
       "production_promotion":False,"performance_campaign":"deferred by user",
       "scope":"native mixed-block ILU(0) on original, reduced and tree-core matrices; focused correctness only"}
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
        names=("/NativeConduitCondensation.cpp","/ImplicitNetworkSolver.cpp","/network_driver.cpp",
               "/test_fv_interface_tree.cpp","/test_fv_conduit_condensation.cpp","/test_fv_implicit_network.cpp",
               "/NativeBlockIlu.cpp","/test_fv_block_ilu.cpp")
        selected=[row for row in commands if row["file"].endswith(names)]
        assert len(selected)==8
        for row in selected:
            flags=row.get("command"," ".join(row.get("arguments",[])))
            assert os.name=="nt" or ("-ffast-math" not in flags and "-fno-fast-math" in flags and "-ffp-contract=off" in flags)
        m["compile_commands"]=selected
        for name,expected in m["source_before"].items():
            dest=out/"source_snapshot"/name; dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(ROOT/name,dest); assert base.digest(dest)==expected,name
        assert sources()==m["source_before"],"source drift during source archive"
        binary=lambda name:build/"bin/Release"/(name+(".exe" if os.name=="nt" else ""))
        engine=Path((build/"tests/verification/fv_implicit_phase0/engine_library_Release.txt").read_text().strip()).resolve()
        artifacts=[engine,binary("fv_implicit_network"),*(binary(name) for name in TARGETS)]
        m["artifacts"]={str(p):base.digest(p) for p in artifacts}; (out/"artifact_snapshot").mkdir()
        for p in artifacts:
            dest=out/"artifact_snapshot"/p.name; shutil.copyfile(p,dest); assert base.digest(dest)==m["artifacts"][str(p)]
        for target in TARGETS:
            work=out/target; work.mkdir()
            with (work/"run.log").open("w") as log:
                process=subprocess.run([str(binary(target)),f"--gtest_output=xml:{work/'results.xml'}"],
                    cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
            xml=ET.parse(work/"results.xml").getroot()
            m["tests"][target]={"returncode":process.returncode,"tests":int(xml.attrib["tests"]),"failures":int(xml.attrib["failures"])}; save()
            assert process.returncode==0 and int(xml.attrib["failures"])==int(xml.attrib["errors"])==0,target
        reference=ROOT/"tests/output/fv_implicit_phase1/2026-10-20_tree_accepted"
        prior=json.loads((reference/"manifest.json").read_text())
        controls={(row["mode"],*previous.key(row)):row for row in prior["networks"]}
        for mode in ("full","reduced","tree_reduced","full_block","reduced_block","tree_reduced_block","fallback_block"):
            for name in ("bed","geometry","virtual","tee_source","loop","reversed"):
                if mode=="fallback_block" and name not in ("tee_source","loop"): continue
                work=out/f"case_{len(m['networks']):03d}_{name}_{mode}"; work.mkdir()
                command=[str(binary("fv_implicit_network")),name,"16","660",".25","1",str(work),"40","1",mode]
                with (work/"run.log").open("w") as log:
                    process=subprocess.run(command,cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
                row=json.loads((work/"summary.json").read_text()); row.update(run=work.name,returncode=process.returncode)
                m["networks"].append(row); save()
                assert process.returncode==0 and row["completed"],row
                row["independent"]=previous.network.audit(work,row)
                row["face_physics"]=previous.physics.face_physics(work,row)
                row["linear_audit"]=audit_linear(work,row)
                row["files"]={p.name:base.digest(p) for p in work.iterdir() if p.suffix in (".csv",".closures")}
                if not mode.endswith("_block"):
                    ref=reference/controls[(mode,*previous.key(row))]["run"]
                    files=("states.csv","nodes.csv","mesh_before.csv","mesh_after.csv","mesh_before.csv.closures","mesh_after.csv.closures")
                    for file in files: assert base.digest(work/file)==base.digest(ref/file),(work,file)
                    m["prior_controls"].append({"run":work.name,"reference":str(ref),"files":files})
                save()
        m["comparisons"]=previous.compare(out,m["networks"])
        m["source_after"]=sources()
        m["later_working_tree_drift"]=[name for name in sorted(m["source_before"].keys()|m["source_after"].keys())
            if m["source_before"].get(name)!=m["source_after"].get(name)]
        for path,expected in m["artifacts"].items(): assert base.digest(Path(path))==expected,path
        m["test_cases"]=sum(row["tests"] for row in m["tests"].values())
        m["status"]="captured"
    except Exception as exc:
        m.update(status="failed",error=str(exc)); raise
    finally: save()
    review(out)


if __name__=="__main__": main()
