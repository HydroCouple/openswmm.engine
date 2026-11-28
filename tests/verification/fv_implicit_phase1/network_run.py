#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Capture the bounded native implicit network increment and independent ledgers."""
import argparse
import csv
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET
import run as reach

ROOT=reach.ROOT
base=reach.base
TARGETS=dict(reach.TARGETS,test_engine_fv_implicit_network="test_fv_implicit_network.cpp")


def sources():
    result=reach.sources()
    p=ROOT/"tests/unit/engine/test_fv_implicit_network.cpp"
    result[str(p.relative_to(ROOT))]=base.digest(p)
    return dict(sorted(result.items()))


def matrix():
    for case in ("bed","geometry","virtual","tee","loop","reversed"):
        for cells in (4,16):
            for c in (150,660): yield case,cells,c,10,10,40,False
    for case in ("bed","geometry","virtual","tee","loop","reversed","tee_source"):
        for cells in (4,8,16):
            for c in (150,660): yield case,cells,c,.25,1,40,True
    for case in ("tee","loop"):
        for restart in (20,80): yield case,16,660,.25,1,restart,True


def audit(work,row):
    for suffix in ("", ".closures"):
        assert base.digest(work/("mesh_before.csv"+suffix))==base.digest(work/("mesh_after.csv"+suffix))
    with (work/"mesh_before.csv").open() as stream: mesh_rows=list(csv.reader(stream))
    mesh={r[0]:[float(v) for v in r[1:]] for r in mesh_rows if r[0]!="section"}
    sections={int(r[1]):[float(v) for v in r[2:]] for r in mesh_rows if r[0]=="section"}
    with (work/"states.csv").open() as stream: states=list(csv.DictReader(stream))
    with (work/"trace.csv").open() as stream: trace=list(csv.DictReader(stream))
    with (work/"faces.csv").open() as stream: faces=list(csv.DictReader(stream))
    with (work/"nodes.csv").open() as stream: nodes=list(csv.DictReader(stream))
    n=row["cells"]; nf=len(mesh["face_cl"]); nn=len(mesh["node_kind"])
    assert len(states)==n*(row["steps"]+1) and len(trace)==row["steps"] and len(faces)==nf*row["steps"]
    assert len(nodes)==nn*(row["steps"]+1)
    assert all(all(math.isfinite(float(v)) for v in r.values()) for r in states+trace+faces+nodes)
    inventory=lambda data: math.fsum(float(r["dx_ft"])*float(r["area_ft2"]) for r in data)
    initial=inventory(states[:n]); cumulative_out=0.; cumulative_source=0.; worst=0.; max_cell=0.; max_node=0.; max_momentum=0.
    for step,t in enumerate(trace,1):
        old=states[(step-1)*n:step*n]; new=states[step*n:(step+1)*n]; fs=faces[(step-1)*nf:step*nf]
        dt=float(t["dt_s"]); mass=[float(f["mass_cfs"]) for f in fs]
        external=0.; junction=[0.]*nn
        for f,q in enumerate(mass):
            j=int(mesh["face_node"][f]); l=int(mesh["face_cl"][f])
            if j>=0:
                incoming=(1 if l>=0 else -1)*q
                if int(mesh["node_kind"][j])==3: external+=incoming
                else: junction[j]+=incoming
        cell_sources=[0.]*n; node_sources=[0.]*nn
        if row["case"]=="tee_source": node_sources[1]=.1; cell_sources[n//2]=.02
        for i,(a,b) in enumerate(zip(old,new)):
            outgoing=0.; force=0.
            for face_key,side_key in (("cell_face0","cell_side0"),("cell_face1","cell_side1")):
                f=int(mesh[face_key][i]); side=int(mesh[side_key][i]); sign=1 if side==0 else -1
                outgoing+=sign*mass[f]
                direction=mesh["face_dir_l" if side==0 else "face_dir_r"][f]
                correction=float(fs[f]["correction_left" if side==0 else "correction_right"])
                force+=sign*direction*(float(fs[f]["momentum"])+correction)
            rv=float(b["dx_ft"])*(float(b["area_ft2"])-float(a["area_ft2"]))+dt*(outgoing-cell_sources[i])
            budget=1.e-11+1.e-12*max(1.,float(a["dx_ft"])*float(a["area_ft2"]))
            assert abs(rv)<=budget*1.001,(work,i,rv,budget)
            max_cell=max(max_cell,abs(rv))
            q=float(b["discharge_cfs"]); area=float(b["area_ft2"]); dx=float(b["dx_ft"])
            radius=sections[int(mesh["cell_geom"][i])][-1]
            factor=mesh["conduit_rough_factor"][int(mesh["cell_conduit"][i])]
            friction=factor*abs(q/area)*q/(radius*math.cbrt(radius))
            rq=dx*(q-float(a["discharge_cfs"]))+dt*(force+dx*friction)
            momentum_budget=1.e-11+1.e-12*max(1.,dx*abs(float(a["discharge_cfs"])))
            assert abs(rq)<=momentum_budget*1.001,(work,i,rq,momentum_budget)
            max_momentum=max(max_momentum,abs(rq))
        for j,q in enumerate(junction):
            if int(mesh["node_kind"][j])==0:
                r=q+node_sources[j]; max_node=max(max_node,abs(r)); assert abs(r)<1.e-10
                logged=float(nodes[step*nn+j]["junction_residual_cfs"])
                assert abs(r-logged)<1.e-13
        cumulative_out+=dt*external; cumulative_source+=dt*(sum(cell_sources)+sum(node_sources))
        balance=inventory(new)-initial+cumulative_out-cumulative_source
        worst=max(worst,abs(balance)/initial)
        assert abs(float(t["external_out_ft3"])-cumulative_out)<1.e-10
        assert abs(float(t["source_ft3"])-cumulative_source)<1.e-12
        assert abs(float(t["balance_ft3"])-balance)<1.e-9
        assert float(t["scaled_residual"])<=1
    assert worst<=1.e-10
    if not row["moving"]:
        assert row["max_discharge_cfs"]<1.e-9 and row["max_head_departure_ft"]<1.e-9,row
    return {"max_relative_balance":worst,"max_cell_volume_residual_ft3":max_cell,"max_junction_residual_cfs":max_node,"max_momentum_residual_ft4_s":max_momentum}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args(); out=args.output.resolve(); build=args.build_dir.resolve()
    if out.exists(): parser.error("output must be new")
    out.mkdir(parents=True)
    m={"status":"running","source_before":sources(),"tests":{},"networks":[],"reach_controls":[],
       "scope":"standalone fully pressurized SLOT network; no physical node storage or production selection"}
    save=lambda:base.write_json(out/"manifest.json",m)
    save()
    try:
        env,removed=base.clean_environment(); m["cleared_environment"]=removed
        m["head"]=subprocess.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip()
        command=["cmake","--build",str(build),"--config","Release","--parallel","4","--target","fv_implicit_network","fv_implicit_reach",*TARGETS]
        m["build_command"]=command
        with (out/"build.log").open("w") as log: subprocess.run(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=1800)
        assert sources()==m["source_before"],"source drift during build"
        for name in ("CMakeCache.txt","compile_commands.json"): shutil.copyfile(build/name,out/name)
        commands=json.loads((out/"compile_commands.json").read_text())
        selected=[r for r in commands if r["file"].endswith(("/ImplicitNetworkSolver.cpp","/NativeGmres.cpp","/test_fv_implicit_network.cpp","/network_driver.cpp"))]
        assert len(selected)==4
        for r in selected:
            flags=r.get("command"," ".join(r.get("arguments",[])))
            assert os.name=="nt" or ("-ffast-math" not in flags and "-fno-fast-math" in flags and "-ffp-contract=off" in flags)
        m["compile_commands"]=selected
        for name,expected in m["source_before"].items():
            dest=out/"source_snapshot"/name; dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(ROOT/name,dest); assert base.digest(dest)==expected,name
        assert sources()==m["source_before"],"source drift during archive"
        engine=Path((build/"tests/verification/fv_implicit_phase0/engine_library_Release.txt").read_text().strip()).resolve()
        binary=lambda name:build/"bin/Release"/(name+(".exe" if os.name=="nt" else ""))
        artifacts=[engine,binary("fv_implicit_network"),binary("fv_implicit_reach"),*(binary(n) for n in TARGETS)]
        m["artifacts"]={str(p):base.digest(p) for p in artifacts}; (out/"artifact_snapshot").mkdir()
        for p in artifacts:
            dest=out/"artifact_snapshot"/p.name; shutil.copyfile(p,dest); assert base.digest(dest)==m["artifacts"][str(p)]
        for name in TARGETS:
            work=out/name; work.mkdir(); command=[str(binary(name)),f"--gtest_output=xml:{work/'results.xml'}"]
            if name in reach.previous.FILTERS: command += [f"--gtest_filter={reach.previous.FILTERS[name]}"]
            with (work/"run.log").open("w") as log: process=subprocess.run(command,cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
            xml=ET.parse(work/"results.xml").getroot()
            m["tests"][name]={"returncode":process.returncode,"tests":int(xml.attrib["tests"]),"failures":int(xml.attrib["failures"])}; save()
            assert process.returncode==0 and int(xml.attrib["failures"])==0,name
        for i,(case,cells,c,cap,end,restart,moving) in enumerate(matrix()):
            work=out/f"network_{i:03d}_{case}"; work.mkdir()
            command=[str(binary("fv_implicit_network")),case,str(cells),str(c),str(cap),str(end),str(work),str(restart),str(int(moving))]
            with (work/"run.log").open("w") as log: process=subprocess.run(command,cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
            row=json.loads((work/"summary.json").read_text()); row.update(run=work.name,returncode=process.returncode)
            m["networks"].append(row); save()
            assert process.returncode==0 and row["completed"],row
            row["independent"]=audit(work,row)
            row["files"]={p.name:base.digest(p) for p in work.iterdir() if p.suffix in (".csv",".closures")}
            save()
        reference=ROOT/"tests/output/fv_implicit_phase1/2026-10-20_native_gmres_final"
        prior=json.loads((reference/"manifest.json").read_text())
        for i,r in enumerate(v for v in prior["components"] if v["mode"]=="coupled"):
            work=out/f"reach_control_{i:02d}"; work.mkdir()
            command=[str(binary("fv_implicit_reach")),"coupled",r["case"],str(r["cells"]),str(r["celerity_ft_s"]),str(r["dt_cap_s"]),str(r["reached_s"]),str(work),"40"]
            with (work/"run.log").open("w") as log: process=subprocess.run(command,cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
            assert process.returncode==0
            for filename in ("states.csv","mesh_before.csv","mesh_after.csv"):
                assert base.digest(work/filename)==base.digest(reference/r["run"]/filename),(work,filename)
            m["reach_controls"].append({"run":work.name,"reference":str(reference/r["run"]),"states_sha256":base.digest(work/"states.csv")}); save()
        m["source_after"]=sources()
        m["later_working_tree_drift"]=[n for n in sorted(m["source_before"].keys()|m["source_after"].keys()) if m["source_before"].get(n)!=m["source_after"].get(n)]
        for p,expected in m["artifacts"].items(): assert base.digest(Path(p))==expected,p
        m["test_cases"]=sum(r["tests"] for r in m["tests"].values())
        m["status"]="captured"; m["production_promotion"]=False
    except Exception as exc:
        m.update(status="failed",error=str(exc)); raise
    finally: save()
    print(f"Captured {len(m['networks'])} networks, {m['test_cases']} native cases and {len(m['reach_controls'])} unchanged reach controls.")


if __name__=="__main__": main()
