#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Capture native exact conduit condensation versus the unchanged graph solve."""
import argparse
import csv
import ctypes
import struct
import json
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET
import network_run as network
import network_review as physics

ROOT=network.ROOT
base=network.base
TARGETS=dict(network.TARGETS,test_engine_fv_conduit_condensation="test_fv_conduit_condensation.cpp")


def sources():
    result=network.sources()
    p=ROOT/"tests/unit/engine/test_fv_conduit_condensation.cpp"
    result[str(p.relative_to(ROOT))]=base.digest(p)
    return dict(sorted(result.items()))


def cases():
    for mode in ("full","condensed","reduced"):
        for values in network.matrix(): yield mode,*values
    for case in ("geometry","tee_source","loop","reversed"):
        yield "fallback",case,16,660,.25,1,40,True
    for mode in ("full","condensed","reduced"):
        for case in ("tee","loop"):
            for cells in (64,256): yield mode,case,cells,660,.25,1,40,True


def audit_linear(work,row):
    with (work/"linear.csv").open() as stream: lines=list(csv.DictReader(stream))
    assert len(lines)==row["newton"]
    with (work/"mesh_before.csv").open() as stream:
        mesh={r[0]:[float(v) for v in r[1:]] for r in csv.reader(stream) if r[0]!="section"}
    with (work/"nodes.csv").open() as stream: nodes=list(csv.DictReader(stream))[:len(mesh["node_kind"])]
    unknown=sum(int(mesh["node_kind"][j])==0 and not int(r["fixed"]) for j,r in enumerate(nodes))
    protected=4*len(mesh["conduit_cell_count"])+unknown
    total_iterations=0
    for r in lines:
        total_iterations+=int(r["gmres_iterations"])+(int(r["reduced_iterations"]) if int(r["fallback"]) else 0)
        assert int(r["accepted_step"])==1 and int(r["linear_converged"])==1
        assert float(r["full_true_residual"])<=float(r["full_tolerance"])
        assert int(r["buffer_bound_bytes"])<=64*1024*1024
        mode=row["mode"]
        if mode=="full": assert int(r["condensation_status"])==0 and int(r["fallback"])==0
        elif mode=="fallback": assert int(r["condensation_status"])==6 and int(r["fallback"])==1
        else:
            assert int(r["condensation_status"])==1 and int(r["fallback"])==0
            assert int(r["retained"])==protected
            assert int(r["eliminated"])+protected==2*row["cells"]+unknown
            assert int(r["schur_entries"])<=1000000
            assert int(r["dense"])==(mode=="condensed")
            assert int(r["reduced_gmres"])==(mode=="reduced")
    assert total_iterations==row["gmres"],(work,total_iterations,row["gmres"])
    return {"corrections":len(lines),"max_true_residual":max((float(r["full_true_residual"]) for r in lines),default=0.),
            "max_residual_ratio":max((float(r["full_true_residual"])/float(r["full_tolerance"]) for r in lines),default=0.)}


def key(row):
    return tuple(row[k] for k in ("case","cells_per_conduit","celerity_ft_s","dt_cap_s","restart","moving"))


def compare(out,rows):
    full={key(r):r for r in rows if r["mode"]=="full"}; results=[]
    for row in rows:
        if row["mode"]=="full": continue
        ref=full[key(row)]
        def final(r):
            with (out/r["run"]/"states.csv").open() as stream: return list(csv.DictReader(stream))[-r["cells"]:]
        a,b=final(row),final(ref)
        errors={field:max(abs(float(x[field])-float(y[field])) for x,y in zip(a,b)) for field in ("depth_ft","area_ft2","discharge_cfs")}
        assert errors["depth_ft"]<=1.e-8 and errors["area_ft2"]<=1.e-10 and errors["discharge_cfs"]<=1.e-8,(row,errors)
        for name in ("mesh_before.csv","mesh_before.csv.closures","mesh_after.csv","mesh_after.csv.closures"):
            assert base.digest(out/row["run"]/name)==base.digest(out/ref["run"]/name)
        results.append({"run":row["run"],"reference":ref["run"],"errors":errors})
    return results



def legacy_closure_members(path):
    """Canonicalize the preceding capture's native POD, excluding padding."""
    scalar=("y_full","dh","inv_dh","a_crown","t_slot","i1_crown","r_full",
            "inv_a_crown_n","c2","c1","p0","p1")
    class Closure(ctypes.Structure):
        _fields_=[(name,ctypes.c_double) for name in scalar]+[("kind",ctypes.c_uint8)]+[
            (name,ctypes.c_double*129) for name in ("A","M","I1","R")]+[("j_of_a",ctypes.c_int16*129)]
    data=path.read_bytes();size=ctypes.sizeof(Closure)
    assert size==4496 and len(data)%size==0
    result=bytearray()
    for offset in range(0,len(data),size):
        c=Closure.from_buffer_copy(data[offset:offset+size])
        result.extend(struct.pack("=12d",*(getattr(c,name) for name in scalar)))
        result.extend(struct.pack("=B",c.kind))
        for name in ("A","M","I1","R"):result.extend(struct.pack("=129d",*getattr(c,name)))
        result.extend(struct.pack("=129h",*c.j_of_a))
    return bytes(result)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir",type=Path,required=True); parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args(); build=args.build_dir.resolve(); out=args.output.resolve()
    if out.exists(): parser.error("output must be new")
    out.mkdir(parents=True)
    m={"status":"running","source_before":sources(),"tests":{},"networks":[],"prior_full_controls":[],
       "scope":"bounded first-order fully pressurized SLOT network; exact eligible conduit interior condensation",
       "mesh_closure_encoding":"fieldwise-native-v1 (all semantic members; excludes POD alignment padding)"}
    save=lambda:base.write_json(out/"manifest.json",m)
    save()
    try:
        env,removed=base.clean_environment(); m["cleared_environment"]=removed
        m["head"]=subprocess.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip()
        command=["cmake","--build",str(build),"--config","Release","--parallel","4","--target","fv_implicit_network",*TARGETS]
        m["build_command"]=command
        with (out/"build.log").open("w") as log: subprocess.run(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=1800)
        assert sources()==m["source_before"],"source drift during build"
        for name in ("CMakeCache.txt","compile_commands.json"): shutil.copyfile(build/name,out/name)
        commands=json.loads((out/"compile_commands.json").read_text())
        selected=[r for r in commands if r["file"].endswith(("/NativeConduitCondensation.cpp","/ImplicitNetworkSolver.cpp","/test_fv_conduit_condensation.cpp","/network_driver.cpp"))]
        assert len(selected)==4
        for r in selected:
            flags=r.get("command"," ".join(r.get("arguments",[])))
            assert os.name=="nt" or ("-ffast-math" not in flags and "-fno-fast-math" in flags and "-ffp-contract=off" in flags)
        m["compile_commands"]=selected
        for name,expected in m["source_before"].items():
            dest=out/"source_snapshot"/name; dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(ROOT/name,dest); assert base.digest(dest)==expected,name
        assert sources()==m["source_before"],"source drift during source archive"
        engine=Path((build/"tests/verification/fv_implicit_phase0/engine_library_Release.txt").read_text().strip()).resolve()
        binary=lambda name:build/"bin/Release"/(name+(".exe" if os.name=="nt" else ""))
        artifacts=[engine,binary("fv_implicit_network"),*(binary(n) for n in TARGETS)]
        m["artifacts"]={str(p):base.digest(p) for p in artifacts}; (out/"artifact_snapshot").mkdir()
        for p in artifacts:
            dest=out/"artifact_snapshot"/p.name; shutil.copyfile(p,dest); assert base.digest(dest)==m["artifacts"][str(p)]
        for name in TARGETS:
            work=out/name; work.mkdir(); command=[str(binary(name)),f"--gtest_output=xml:{work/'results.xml'}"]
            if name in network.reach.previous.FILTERS: command += [f"--gtest_filter={network.reach.previous.FILTERS[name]}"]
            with (work/"run.log").open("w") as log: process=subprocess.run(command,cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
            xml=ET.parse(work/"results.xml").getroot()
            m["tests"][name]={"returncode":process.returncode,"tests":int(xml.attrib["tests"]),"failures":int(xml.attrib["failures"])}; save()
            assert process.returncode==0 and int(xml.attrib["failures"])==0,name
        reference=ROOT/"tests/output/fv_implicit_phase1/2026-10-20_balanced_network_final"
        prior=json.loads((reference/"manifest.json").read_text()); controls={key(r):r for r in prior["networks"]}
        for i,(mode,case,cells,c,cap,end,restart,moving) in enumerate(cases()):
            work=out/f"case_{i:03d}_{case}_{mode}"; work.mkdir()
            command=[str(binary("fv_implicit_network")),case,str(cells),str(c),str(cap),str(end),str(work),str(restart),str(int(moving)),mode]
            with (work/"run.log").open("w") as log: process=subprocess.run(command,cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
            row=json.loads((work/"summary.json").read_text()); row.update(run=work.name,returncode=process.returncode)
            m["networks"].append(row);save()
            assert process.returncode==0 and row["completed"],row
            row["independent"]=network.audit(work,row)
            row["face_physics"]=physics.face_physics(work,row)
            row["linear_audit"]=audit_linear(work,row)
            row["files"]={p.name:base.digest(p) for p in work.iterdir() if p.suffix in (".csv",".closures")}
            if mode=="full" and key(row) in controls:
                ref=reference/controls[key(row)]["run"]
                for name in ("states.csv","nodes.csv","mesh_before.csv","mesh_after.csv"):
                    assert base.digest(work/name)==base.digest(ref/name),(work,name)
                for name in ("mesh_before.csv.closures","mesh_after.csv.closures"):
                    assert (work/name).read_bytes()==legacy_closure_members(ref/name),(work,name)
                m["prior_full_controls"].append({"run":work.name,"reference":str(ref),"all_closure_members_equal":True})
            save()
        m["comparisons"]=compare(out,m["networks"])
        m["source_after"]=sources()
        m["later_working_tree_drift"]=[n for n in sorted(m["source_before"].keys()|m["source_after"].keys()) if m["source_before"].get(n)!=m["source_after"].get(n)]
        for p,expected in m["artifacts"].items(): assert base.digest(Path(p))==expected,p
        m["test_cases"]=sum(r["tests"] for r in m["tests"].values())
        m["status"]="captured";m["production_promotion"]=False
    except Exception as exc:
        m.update(status="failed",error=str(exc));raise
    finally:save()
    print(f"Captured {len(m['networks'])} networks, {m['test_cases']} native cases and {len(m['prior_full_controls'])} prior controls.")


if __name__=="__main__":main()
